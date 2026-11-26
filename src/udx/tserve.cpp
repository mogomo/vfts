// vfts.tserve(* USING PARAMETERS index, endpoint, token, workers, k, mode, ...) OVER()
//     FROM <schema>.<index>_snap
//
// A long-running statement that answers searches over a channel instead of one statement per
// query. It is meant for tools/vfts_broker, which opens a normal Vertica session for a user (so
// Vertica checks the login, the grants on the view and the resource pool), runs this statement in
// it, and then sends that user's searches over the channel.
//
// The statement reads its view like tsearch (access token, generation check, journal and policy
// masks, the live tail as of
// the start of the statement: what it serves is a snapshot), connects to the Unix socket
// `endpoint`, presents `token` (made by the broker for this statement only), and answers frames
// until the broker closes the socket or the statement is canceled. Output: one row, the number of
// searches answered.
//
// Frames (host byte order):
//   request   u32 length, u32 id, query (UTF-8, length - 4 bytes)
//   response  u32 length, u32 id, i32 n, n x (i64 doc_id, f32 score)   n = -1: an error message follows
// A request whose text starts with the byte 0 carries options (attributes, attribute.h):
//   0, u8 op (0 search, 1 count, 2 count by), u32 k (0: the statement's k), u16 length + filter,
//   u16 length + by (the attribute to count by), then the query
//   response of a search: as above; of a count: i32 n buckets, n x (u64 count, u16 length + value)
//   (a count: one bucket with an empty value; count by: one per value with matches, value order)
//
// The searches run on `workers` threads (default: one per core) that this statement starts and
// joins before it ends; each search uses one thread, each count (unless threads is given) its fair
// share of the engine's pool. They run engine code only, never the SDK.
#include "udx_common.h"
#include "search_options.h"
#include "view_input.h"
#include "../engine/analyzer.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <list>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

using namespace Vertica;
using namespace vfts_udx;

namespace {

struct Job {
    std::uint32_t id;
    std::string query;
};

// A request: the query alone, or (first byte 0) options before it.
struct Request {
    int op = 0;                 // 0 search, 1 count, 2 count by
    std::uint32_t k = 0;
    std::string filter, by, query;
};

Request decode(const std::string &b)
{
    Request r;
    if (b.empty() || b[0] != '\0') { r.query = b; return r; }
    std::size_t at = 1;
    auto need = [&](std::size_t n) { if (at + n > b.size()) fail("request frame too short"); };
    need(5);
    r.op = static_cast<unsigned char>(b[at++]);
    std::memcpy(&r.k, b.data() + at, 4);
    at += 4;
    for (std::string *f : {&r.filter, &r.by}) {
        need(2);
        std::uint16_t l;
        std::memcpy(&l, b.data() + at, 2);
        at += 2;
        need(l);
        f->assign(b, at, l);
        at += l;
    }
    r.query = b.substr(at);
    if (r.op > 2) fail("unknown request kind " + std::to_string(r.op));
    if ((r.op == 2) != !r.by.empty()) fail(r.op == 2 ? "count by: attribute missing" : "by: only with count by");
    if (r.k > 1000000) fail("k must be 1 to 1000000");
    return r;
}

// The answers of counts, kept while the statement runs (Elasticsearch's shard request cache keeps
// the answers of requests without hits the same way, until the next refresh). The statement
// searches one snapshot, so an answer stays right while it runs; the attribute files a request
// reads are part of the key, since refresh_attribute may replace them meanwhile. LRU, by bytes.
class AnswerCache {
public:
    explicit AnswerCache(std::size_t max_bytes) : max_(max_bytes) {}
    bool get(const std::string &key, std::int32_t &n, std::string &body)
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        lru_.splice(lru_.begin(), lru_, it->second);
        n = it->second->n;
        body = it->second->body;
        return true;
    }
    void put(const std::string &key, std::int32_t n, const std::string &body)
    {
        const std::size_t size = key.size() * 2 + body.size() + 128;
        if (size > max_ / 8) return;
        std::lock_guard<std::mutex> l(mu_);
        if (map_.count(key)) return;
        lru_.push_front({key, n, body});
        map_[key] = lru_.begin();
        bytes_ += size;
        while (bytes_ > max_ && !lru_.empty()) {
            const Entry &e = lru_.back();
            bytes_ -= e.key.size() * 2 + e.body.size() + 128;
            map_.erase(e.key);
            lru_.pop_back();
        }
    }

private:
    struct Entry { std::string key; std::int32_t n; std::string body; };
    std::mutex mu_;
    std::list<Entry> lru_;
    std::unordered_map<std::string, std::list<Entry>::iterator> map_;
    std::size_t bytes_ = 0;
    const std::size_t max_;
};

bool write_all(int fd, const char *p, std::size_t n)
{
    while (n) {
        const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

class TServe : public TransformFunction {
public:
    void processPartition(ServerInterface &srv, PartitionReader &in, PartitionWriter &out) override
    {
        try {
            run(srv, in, out);
        } catch (const std::exception &e) {
            vt_report_error(0, "tserve: %s", e.what());
        }
    }

private:
    void run(ServerInterface &srv, PartitionReader &in, PartitionWriter &out)
    {
        Settings s(srv);
        const std::string index = read_index_name(s);
        const std::string endpoint = s.text("endpoint", "");
        const std::string token = s.text("token", "");
        if (endpoint.empty() || endpoint.size() >= sizeof(sockaddr_un::sun_path)) fail("parameter endpoint (a Unix socket path) is required");
        if (token.size() < 16) fail("parameter token (at least 16 characters) is required");
        {
            // only a broker's socket: <dir>/vfts_broker.<name>.sock, a socket file (so that the
            // function cannot be pointed at other local services)
            const std::size_t sl = endpoint.rfind('/');
            const std::string base = sl == std::string::npos ? endpoint : endpoint.substr(sl + 1);
            struct stat st{};
            static const std::string pre = "vfts_broker.", suf = ".sock";   // and a name of at least one character
            if (base.rfind(pre, 0) != 0 || base.size() < pre.size() + 1 + suf.size() || base.compare(base.size() - suf.size(), suf.size(), suf) != 0 ||
                ::stat(endpoint.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode))
                fail("endpoint must be the socket of vfts_broker (<dir>/vfts_broker.<name>.sock)");
        }
        std::int64_t workers = s.integer("workers", 0);
        if (workers < 0 || workers > 1024) fail("workers must be 0 (one per core) to 1024");
        if (workers == 0) workers = std::max<std::int64_t>(1, std::thread::hardware_concurrency());
        const vfts::SearchOptions o = parse_search_options(s, 1);
        // a count reads every match: without threads given, it takes the pool's fair share of the
        // cores (all of them when it runs alone, about one each under load); a search takes one
        const int count_threads = s.has("threads") ? o.threads : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        const std::string cache_dir = resolve_cache_dir(s);
        const std::string stmt_filter = s.text("filter", "");     // for requests without their own
        const bool s_filter_cache = s.boolean("filter_cache", true);
        const bool request_cache = s.boolean("request_cache", true);
        AnswerCache answers(64u << 20);

        ViewInput v;
        v.read(in, false);
        auto li = v.open(resolve_cache_dir(s), index, srv.getUserName());
        v.warn_long_tail(srv, s, index);
        const vfts::AnalyzerOptions aopts = parse_index_options(li->options);
        std::unique_ptr<vfts::Searcher> sp = v.searcher(*li);
        vfts::Searcher &searcher = *sp;
        searcher.ready();

        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) fail("socket: " + std::string(std::strerror(errno)));
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::memcpy(a.sun_path, endpoint.data(), endpoint.size());
        if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
            const std::string e = std::strerror(errno);
            ::close(fd);
            fail("cannot connect to " + endpoint + ": " + e);
        }
        const std::string hello = "VFTS1 " + token + "\n";
        if (!write_all(fd, hello.data(), hello.size())) { ::close(fd); fail("channel closed at the start"); }

        std::mutex mu, wmu;
        std::condition_variable cv;
        std::deque<Job> jobs;
        bool stop = false;
        std::atomic<std::uint64_t> served{0};
        std::vector<std::thread> pool;
        for (std::int64_t w = 0; w < workers; ++w)
            pool.emplace_back([&] {
                vfts::Analyzer analyzer(aopts);
                std::string frame;
                for (;;) {
                    Job j;
                    {
                        std::unique_lock<std::mutex> l(mu);
                        cv.wait(l, [&] { return stop || !jobs.empty(); });
                        if (jobs.empty()) return;
                        j = std::move(jobs.front());
                        jobs.pop_front();
                    }
                    frame.assign(12, '\0');
                    std::int32_t n;
                    try {
                        Request r = decode(j.query);
                        vfts::SearchOptions ro = o;
                        if (r.k) ro.k = r.k;
                        if (r.op != 0) ro.threads = count_threads;
                        AttrSearch as;
                        as.open(cache_dir, index, r.filter.empty() ? stmt_filter : r.filter, r.by);
                        std::string ckey;
                        if (request_cache && r.op != 0) {
                            ckey = j.query + '\x1d' + as.key();
                            std::string body;
                            if (answers.get(ckey, n, body)) {
                                frame += body;
                                goto answer;
                            }
                        }
                        as.apply(s_filter_cache, searcher, ro);
                        if (r.op == 0) {
                            const std::vector<vfts::Hit> hits = searcher.search(r.query, ro, analyzer);
                            n = static_cast<std::int32_t>(hits.size());
                            for (const vfts::Hit &h : hits) {
                                frame.append(reinterpret_cast<const char *>(&h.doc_id), 8);
                                frame.append(reinterpret_cast<const char *>(&h.score), 4);
                            }
                        } else if (r.op == 1) {
                            const std::uint64_t c = searcher.count(r.query, ro, analyzer);
                            const std::uint16_t z = 0;
                            n = 1;
                            frame.append(reinterpret_cast<const char *>(&c), 8);
                            frame.append(reinterpret_cast<const char *>(&z), 2);
                        } else {
                            const std::vector<std::uint64_t> c = searcher.count_by(r.query, ro, as.by_codes, as.by->n_codes(), analyzer);
                            n = 0;
                            for (std::uint32_t v = 0; v < c.size(); ++v) {
                                if (!c[v]) continue;
                                const std::string val = as.by->value_string(v);
                                const std::uint16_t vl = static_cast<std::uint16_t>(std::min<std::size_t>(val.size(), 65535));
                                frame.append(reinterpret_cast<const char *>(&c[v]), 8);
                                frame.append(reinterpret_cast<const char *>(&vl), 2);
                                frame.append(val.data(), vl);
                                ++n;
                            }
                        }
                        if (!ckey.empty()) answers.put(ckey, n, frame.substr(12));
                    } catch (const std::exception &e) {
                        n = -1;
                        frame.resize(12);
                        frame += e.what();
                    }
                answer:
                    const std::uint32_t len = static_cast<std::uint32_t>(frame.size() - 4);
                    std::memcpy(&frame[0], &len, 4);
                    std::memcpy(&frame[4], &j.id, 4);
                    std::memcpy(&frame[8], &n, 4);
                    {
                        std::lock_guard<std::mutex> l(wmu);
                        if (!write_all(fd, frame.data(), frame.size())) ::shutdown(fd, SHUT_RDWR);
                    }
                    ++served;
                }
            });

        // the reader: frames in, until the broker closes the channel or the statement is canceled
        std::string buf;
        char tmp[65536];
        for (;;) {
            if (isCanceled()) break;
            pollfd p{fd, POLLIN, 0};
            const int r = ::poll(&p, 1, 200);
            if (r < 0 && errno != EINTR) break;
            if (r <= 0) continue;
            const ssize_t got = ::recv(fd, tmp, sizeof tmp, 0);
            if (got <= 0) break;
            buf.append(tmp, static_cast<std::size_t>(got));
            std::size_t at = 0;
            std::vector<Job> batch;
            while (buf.size() - at >= 8) {
                std::uint32_t len, id;
                std::memcpy(&len, buf.data() + at, 4);
                if (len < 4 || len > 16 * 1024 * 1024) { at = buf.size(); break; }
                if (buf.size() - at < 4 + static_cast<std::size_t>(len)) break;
                std::memcpy(&id, buf.data() + at + 4, 4);
                batch.push_back({id, buf.substr(at + 8, len - 4)});
                at += 4 + len;
            }
            buf.erase(0, at);
            if (!batch.empty()) {
                {
                    std::lock_guard<std::mutex> l(mu);
                    for (Job &j : batch) jobs.push_back(std::move(j));
                }
                if (batch.size() == 1) cv.notify_one(); else cv.notify_all();
            }
        }
        {
            std::lock_guard<std::mutex> l(mu);
            stop = true;
            jobs.clear();
        }
        cv.notify_all();
        for (std::thread &t : pool) t.join();
        ::close(fd);
        out.setInt(0, static_cast<vint>(served.load()));
        out.next();
    }
};

class TServeFactory : public TransformFunctionFactory {
public:
    void getPrototype(ServerInterface &, ColumnTypes &a, ColumnTypes &r) override
    {
        a.addInt();             // qid
        a.addVarchar();         // query
        a.addInt();             // doc_id
        a.addLongVarchar();     // body
        a.addVarchar();         // lang
        a.addVarchar();         // op
        a.addInt();             // gen
        r.addInt();
    }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override { out.addInt("served"); }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        p.addVarchar(128, "index");
        p.addVarchar(256, "endpoint");
        p.addVarchar(256, "token");
        p.addInt("workers");
        p.addBool("request_cache");
        add_search_parameters(p);
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<TServe>(srv.allocator); }
};

} // namespace

RegisterFactory(TServeFactory);
