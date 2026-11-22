// vtext.tserve(* USING PARAMETERS index, endpoint, token, workers, k, mode, ...) OVER()
//     FROM <schema>.<index>_snap
//
// A long-running statement that answers searches over a channel instead of one statement per
// query. It is meant for tools/vtext_broker, which opens a normal Vertica session for a user (so
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
//
// The searches run on `workers` threads (default: one per core) that this statement starts and
// joins before it ends; each search uses one thread. They run engine code only, never the SDK.
#include "udx_common.h"
#include "search_options.h"
#include "view_input.h"
#include "../engine/analyzer.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

using namespace Vertica;
using namespace vtext_udx;

namespace {

struct Job {
    std::uint32_t id;
    std::string query;
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
            // only a broker's socket: <dir>/vtext_broker.<name>.sock, a socket file (so that the
            // function cannot be pointed at other local services)
            const std::size_t sl = endpoint.rfind('/');
            const std::string base = sl == std::string::npos ? endpoint : endpoint.substr(sl + 1);
            struct stat st{};
            if (base.rfind("vtext_broker.", 0) != 0 || base.size() < 18 || base.compare(base.size() - 5, 5, ".sock") != 0 ||
                ::stat(endpoint.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode))
                fail("endpoint must be the socket of vtext_broker (<dir>/vtext_broker.<name>.sock)");
        }
        std::int64_t workers = s.integer("workers", 0);
        if (workers < 0 || workers > 1024) fail("workers must be 0 (one per core) to 1024");
        if (workers == 0) workers = std::max<std::int64_t>(1, std::thread::hardware_concurrency());
        const vtext::SearchOptions o = parse_search_options(s, 1);

        ViewInput v;
        v.read(in, false);
        auto li = v.open(resolve_cache_dir(s), index, srv.getUserName());
        const vtext::AnalyzerOptions aopts = parse_index_options(li->options);
        std::unique_ptr<vtext::Searcher> sp = v.searcher(*li);
        vtext::Searcher &searcher = *sp;
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
        const std::string hello = "VTEXT1 " + token + "\n";
        if (!write_all(fd, hello.data(), hello.size())) { ::close(fd); fail("channel closed at the start"); }

        std::mutex mu, wmu;
        std::condition_variable cv;
        std::deque<Job> jobs;
        bool stop = false;
        std::atomic<std::uint64_t> served{0};
        std::vector<std::thread> pool;
        for (std::int64_t w = 0; w < workers; ++w)
            pool.emplace_back([&] {
                vtext::Analyzer analyzer(aopts);
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
                        const std::vector<vtext::Hit> hits = searcher.search(j.query, o, analyzer);
                        n = static_cast<std::int32_t>(hits.size());
                        for (const vtext::Hit &h : hits) {
                            frame.append(reinterpret_cast<const char *>(&h.doc_id), 8);
                            frame.append(reinterpret_cast<const char *>(&h.score), 4);
                        }
                    } catch (const std::exception &e) {
                        n = -1;
                        frame.resize(12);
                        frame += e.what();
                    }
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
        add_search_parameters(p);
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<TServe>(srv.allocator); }
};

} // namespace

RegisterFactory(TServeFactory);
