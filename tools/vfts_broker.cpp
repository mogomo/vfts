// vfts_broker: an HTTP front end that batches concurrent searches into vfts.tsearch statements.
//
//   vfts_broker --port 9300 --schema bench [--view snap|live] [--k 10] [--param "mode='stem'"] ...
//                [--bind 127.0.0.1] [--mode sql|serve]
//                sql:   [--sessions 4] [--max-batch 32]
//                serve: [--channels 1] [--workers 0] [--socket /tmp/vfts_broker.<port>.sock]
//
// Request:  POST /<index>/_search  {"query": "<vfts query>"}   (HTTP/1.1, keep-alive)
// Response: {"took": <ms>, "batch": <queries in the statement>, "hits": [{"id": <doc_id>, "score": <s>}, ...]}
//
// Every user gets their own Vertica sessions: the credentials of the request (HTTP Basic) are the
// ones vsql logs in with, so Vertica checks the password, the grants, the access policies and the
// resource pool of that user, and every statement is recorded under that user. Without an
// Authorization header the environment's VSQL_USER / VSQL_PASSWORD are used.
//
// Grouping rule: a request is sent at once when one of the user's sessions for that index is idle.
// While all of them are busy, requests wait, and the next session that becomes free takes all of
// them (up to --max-batch) in one statement (the parameter queries). There is no fixed wait.
// Every statement carries the label vfts_broker.
//
// --mode serve: instead of one statement per batch, each of the user's sessions runs one
// vfts.tserve statement (src/udx/tserve.cpp) that stays open; the broker sends that user's
// searches over a Unix socket (0600, the database's OS user), which the statement connects to with
// a token made for it alone. The statement reads the view when it starts: it answers from that
// snapshot until it ends. Every search is then no SQL statement of its own.
//
// The sessions are vsql processes (stdin / stdout pipes), so the broker needs no client library.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <stdexcept>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
    int port = 9300;
    std::string bind = "127.0.0.1";
    std::string schema = "public";
    std::string view = "snap";
    int sessions = 4;
    int max_batch = 32;
    int k = 10;
    std::vector<std::string> params;
    bool serve = false;             // --mode serve: one vfts.tserve statement per session, searches over a channel
    std::string socket_path;        // the channel's Unix socket (serve)
    int channels = 1;               // serve: statements (sessions) per user and index
    int workers = 0;                // serve: search threads per statement (0: one per core)
} cfg;

// ------------------------------------------------------------------ small helpers

std::string json_escape(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o;
}

void put_utf8(std::string &o, unsigned cp)
{
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// The string value of "key" in a flat JSON object (enough for the request bodies above).
bool json_string(const std::string &j, const std::string &key, std::string &out)
{
    const std::string pat = "\"" + key + "\"";
    std::size_t p = j.find(pat);
    if (p == std::string::npos) return false;
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    p = j.find('"', p);
    if (p == std::string::npos) return false;
    out.clear();
    for (std::size_t i = p + 1; i < j.size(); ++i) {
        const char c = j[i];
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (++i >= j.size()) return false;
        switch (j[i]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            if (i + 4 >= j.size()) return false;
            unsigned cp = static_cast<unsigned>(std::strtoul(j.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < j.size() && j[i + 1] == '\\' && j[i + 2] == 'u') {
                const unsigned lo = static_cast<unsigned>(std::strtoul(j.substr(i + 3, 4).c_str(), nullptr, 16));
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            }
            put_utf8(out, cp);
            break;
        }
        default: out += j[i];
        }
    }
    return false;
}

std::string base64_decode(const std::string &in)
{
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char c : in) {
        const std::size_t p = chars.find(c);
        if (p == std::string::npos) break;
        val = (val << 6) + static_cast<int>(p);
        bits += 6;
        if (bits >= 0) { out += static_cast<char>((val >> bits) & 0xFF); bits -= 8; }
    }
    return out;
}

bool safe_name(const std::string &s)
{
    if (s.empty() || s.size() > 128) return false;
    for (char c : s) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

// ------------------------------------------------------------------ a request waiting for its answer

struct Pending {
    std::string query;
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    int status = 200;
    std::string body;
    void finish(int st, std::string b)
    {
        std::lock_guard<std::mutex> l(mu);
        status = st;
        body = std::move(b);
        done = true;
        cv.notify_one();
    }
    void wait()
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [&] { return done; });
    }
};

// ------------------------------------------------------------------ one Vertica session (vsql)

class Session {
    pid_t pid_ = -1;
    FILE *to_ = nullptr, *from_ = nullptr;
    std::uint64_t seq_ = 0;

public:
    Session(const std::string &user, const std::string &password)
    {
        int in[2], out[2];
        if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0) throw std::runtime_error("pipe");
        pid_ = fork();
        if (pid_ == 0) {
            dup2(in[0], 0);
            dup2(out[1], 1);
            dup2(out[1], 2);
            close(in[1]);
            close(out[0]);
            if (!user.empty()) { setenv("VSQL_USER", user.c_str(), 1); setenv("VSQL_PASSWORD", password.c_str(), 1); }
            execlp("vsql", "vsql", "-X", "-A", "-t", "-q", "-F", "\t", "-v", "ON_ERROR_STOP=0", static_cast<char *>(nullptr));
            _exit(127);
        }
        close(in[0]);
        close(out[1]);
        to_ = fdopen(in[1], "w");
        from_ = fdopen(out[0], "r");
        // first statement: the connection is made (and fails here, not on a request)
        std::vector<std::string> lines;
        if (!run("SELECT 1;", lines) || lines.empty() || lines[0] != "1")
            throw std::runtime_error("vsql login failed" + (lines.empty() ? std::string() : ": " + lines[0]));
    }
    ~Session()
    {
        if (to_) fclose(to_);
        if (from_) fclose(from_);
        if (pid_ > 0) { kill(pid_, SIGTERM); waitpid(pid_, nullptr, 0); }
    }

    // Runs one statement; the output lines (rows, or error messages) up to the marker.
    bool run(const std::string &sql, std::vector<std::string> &lines)
    {
        lines.clear();
        const std::string mark = "@@vfb" + std::to_string(++seq_);
        if (std::fputs(sql.c_str(), to_) < 0 || std::fprintf(to_, "\n\\echo %s\n", mark.c_str()) < 0 || std::fflush(to_) != 0) return false;
        char *buf = nullptr;
        std::size_t cap = 0;
        ssize_t n;
        while ((n = getline(&buf, &cap, from_)) >= 0) {
            std::string l(buf, static_cast<std::size_t>(n));
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l == mark) { std::free(buf); return true; }
            lines.push_back(std::move(l));
        }
        std::free(buf);
        return false;
    }
};

// ------------------------------------------------------------------ the sessions of one user for one index

class Lane {
public:
    virtual ~Lane() = default;
    virtual std::string error() = 0;
    virtual void submit(const std::shared_ptr<Pending> &p) = 0;
};

std::string sql_lit(const std::string &s)
{
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "''"; else o += c; }
    return o + "'";
}

// --mode sql: statements of vfts.tsearch with the parameter queries
class SqlLane : public Lane {
    std::string index_, user_, password_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Pending>> queue_;
    std::vector<std::thread> threads_;
    std::atomic<int> ready_{0};
    std::string error_;

    static std::string lit(const std::string &s)
    {
        std::string o = "'";
        for (char c : s) { if (c == '\'') o += "''"; else o += c; }
        return o + "'";
    }

    void serve(int no)
    {
        std::unique_ptr<Session> s;
        try { s.reset(new Session(user_, password_)); }
        catch (const std::exception &e) {
            std::lock_guard<std::mutex> l(mu_);
            error_ = e.what();
        }
        ++ready_;
        cv_.notify_all();
        std::vector<std::string> lines;
        for (;;) {
            std::vector<std::shared_ptr<Pending>> batch;
            {
                std::unique_lock<std::mutex> l(mu_);
                cv_.wait(l, [&] { return !queue_.empty(); });
                while (!queue_.empty() && batch.size() < static_cast<std::size_t>(cfg.max_batch)) {
                    batch.push_back(queue_.front());
                    queue_.pop_front();
                }
            }
            if (!s) { for (auto &p : batch) p->finish(503, "{\"error\": \"" + json_escape(error_) + "\"}"); continue; }
            std::string qs;
            for (std::size_t i = 0; i < batch.size(); ++i) {
                std::string q = batch[i]->query;
                for (char &c : q) if (c == '\n' || c == '\t' || c == '\r') c = ' ';
                qs += std::to_string(i) + "\t" + q + "\n";
            }
            std::string sql = "SELECT /*+LABEL(vfts_broker)*/ qid, doc_id, score FROM (SELECT vfts.tsearch(* USING PARAMETERS index=" + lit(index_) +
                              ", queries=" + lit(qs) + ", k=" + std::to_string(cfg.k);
            for (const std::string &p : cfg.params) sql += ", " + p;
            sql += ") OVER() FROM " + cfg.schema + "." + index_ + "_" + cfg.view + ") r;";
            const auto t0 = Clock::now();
            const bool ok = s->run(sql, lines);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            std::vector<std::string> hits(batch.size());
            std::string err;
            for (const std::string &l : lines) {
                const std::size_t a = l.find('\t'), b = a == std::string::npos ? a : l.find('\t', a + 1);
                if (b == std::string::npos) { if (err.empty() && !l.empty()) err = l; continue; }
                const std::size_t qi = std::strtoull(l.c_str(), nullptr, 10);
                if (qi >= batch.size()) continue;
                std::string &h = hits[qi];
                if (!h.empty()) h += ", ";
                h += "{\"id\": " + l.substr(a + 1, b - a - 1) + ", \"score\": " + l.substr(b + 1) + "}";
            }
            if (!ok) err = "the Vertica session ended";
            char took[64];
            std::snprintf(took, sizeof took, "%.3f", ms);
            for (std::size_t i = 0; i < batch.size(); ++i) {
                if (!err.empty()) batch[i]->finish(err.find("ERROR") != std::string::npos ? 400 : 500, "{\"error\": \"" + json_escape(err) + "\"}");
                else batch[i]->finish(200, std::string("{\"took\": ") + took + ", \"batch\": " + std::to_string(batch.size()) + ", \"hits\": [" + hits[i] + "]}");
            }
            if (!ok) { std::fprintf(stderr, "session %d of %s ended; reconnecting\n", no, index_.c_str()); try { s.reset(new Session(user_, password_)); } catch (...) { s.reset(); } }
        }
    }

public:
    SqlLane(std::string index, std::string user, std::string password)
        : index_(std::move(index)), user_(std::move(user)), password_(std::move(password))
    {
        for (int i = 0; i < cfg.sessions; ++i) threads_.emplace_back([this, i] { serve(i); });
        std::unique_lock<std::mutex> l(mu_);
        cv_.wait(l, [&] { return ready_.load() == cfg.sessions; });
    }
    // the sessions live as long as the broker
    ~SqlLane() override { for (auto &t : threads_) t.detach(); }

    std::string error() override { std::lock_guard<std::mutex> l(mu_); return error_; }

    void submit(const std::shared_ptr<Pending> &p) override
    {
        {
            std::lock_guard<std::mutex> l(mu_);
            queue_.push_back(p);
        }
        cv_.notify_one();
    }
};

// ------------------------------------------------------------------ --mode serve

// The Unix socket the tserve statements connect to; a statement presents its one-time token.
class Acceptor {
    int fd_ = -1;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, int> arrived_;        // token -> connection
    std::set<std::string> expected_;

public:
    void start()
    {
        ::unlink(cfg.socket_path.c_str());
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::snprintf(a.sun_path, sizeof a.sun_path, "%s", cfg.socket_path.c_str());
        if (::bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(fd_, 64) != 0) { std::perror("unix socket"); std::exit(1); }
        ::chmod(cfg.socket_path.c_str(), 0600);      // only the database's OS user (the broker runs as it)
        std::thread([this] {
            for (;;) {
                const int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
                if (c < 0) continue;
                std::thread([this, c] { hello(c); }).detach();
            }
        }).detach();
    }
    void expect(const std::string &token) { std::lock_guard<std::mutex> l(mu_); expected_.insert(token); }
    void forget(const std::string &token) { std::lock_guard<std::mutex> l(mu_); expected_.erase(token); auto it = arrived_.find(token); if (it != arrived_.end()) { ::close(it->second); arrived_.erase(it); } }
    // the connection that presented token, or -1 after the timeout
    int wait(const std::string &token, int seconds)
    {
        std::unique_lock<std::mutex> l(mu_);
        if (!cv_.wait_for(l, std::chrono::seconds(seconds), [&] { return arrived_.count(token) > 0; })) return -1;
        const int c = arrived_[token];
        arrived_.erase(token);
        expected_.erase(token);
        return c;
    }

private:
    void hello(int c)
    {
        std::string line;
        char ch;
        while (line.size() < 300 && ::recv(c, &ch, 1, 0) == 1 && ch != '\n') line += ch;
        static const std::string kHello = "VFTS1 ";   // tserve.cpp sends kHello + token
        const std::string tok = line.rfind(kHello, 0) == 0 ? line.substr(kHello.size()) : std::string();
        std::lock_guard<std::mutex> l(mu_);
        if (tok.empty() || !expected_.count(tok)) { ::close(c); return; }   // unknown or used token
        expected_.erase(tok);
        arrived_[tok] = c;
        cv_.notify_all();
    }
} acceptor;

std::string random_token()
{
    unsigned char b[24];
    FILE *f = std::fopen("/dev/urandom", "rb");
    if (!f || std::fread(b, 1, sizeof b, f) != sizeof b) { if (f) std::fclose(f); throw std::runtime_error("no /dev/urandom"); }
    std::fclose(f);
    std::string t;
    char h[3];
    for (unsigned char x : b) { std::snprintf(h, sizeof h, "%02x", x); t += h; }
    return t;
}

// One tserve statement in one session: requests are frames on its channel, answered in any order.
class Channel {
    std::unique_ptr<Session> s_;
    int fd_ = -1;
    std::mutex wmu_, mu_;
    std::map<std::uint32_t, std::shared_ptr<Pending>> waiting_;
    std::uint32_t next_ = 0;
    std::thread reader_, stmt_;
    std::atomic<bool> alive_{false};
    std::string error_;

    void fail_all(const std::string &why)
    {
        std::map<std::uint32_t, std::shared_ptr<Pending>> w;
        {
            std::lock_guard<std::mutex> l(mu_);
            w.swap(waiting_);
            if (error_.empty()) error_ = why;
        }
        for (auto &x : w) x.second->finish(503, "{\"error\": \"" + json_escape(why) + "\"}");
    }

    void read_loop()
    {
        std::string buf;
        char tmp[65536];
        for (;;) {
            const ssize_t r = ::recv(fd_, tmp, sizeof tmp, 0);
            if (r <= 0) break;
            buf.append(tmp, static_cast<std::size_t>(r));
            std::size_t at = 0;
            while (buf.size() - at >= 12) {
                std::uint32_t len, id;
                std::int32_t n;
                std::memcpy(&len, buf.data() + at, 4);
                if (buf.size() - at < 4 + static_cast<std::size_t>(len)) break;
                std::memcpy(&id, buf.data() + at + 4, 4);
                std::memcpy(&n, buf.data() + at + 8, 4);
                std::shared_ptr<Pending> p;
                {
                    std::lock_guard<std::mutex> l(mu_);
                    auto it = waiting_.find(id);
                    if (it != waiting_.end()) { p = it->second; waiting_.erase(it); }
                }
                if (p) {
                    if (n < 0) {
                        p->finish(400, "{\"error\": \"" + json_escape(buf.substr(at + 12, len - 8)) + "\"}");
                    } else {
                        std::string body = "{\"hits\": [";
                        char item[96];
                        for (std::int32_t i = 0; i < n; ++i) {
                            std::int64_t doc;
                            float sc;
                            std::memcpy(&doc, buf.data() + at + 12 + 12 * static_cast<std::size_t>(i), 8);
                            std::memcpy(&sc, buf.data() + at + 20 + 12 * static_cast<std::size_t>(i), 4);
                            std::snprintf(item, sizeof item, "%s{\"id\": %lld, \"score\": %.9g}", i ? ", " : "", static_cast<long long>(doc), static_cast<double>(sc));
                            body += item;
                        }
                        body += "]}";
                        p->finish(200, std::move(body));
                    }
                }
                at += 4 + len;
            }
            buf.erase(0, at);
        }
        alive_ = false;
        fail_all("the vfts.tserve statement ended");
    }

public:
    Channel(const std::string &index, const std::string &user, const std::string &password)
    {
        s_.reset(new Session(user, password));
        const std::string token = random_token();
        acceptor.expect(token);
        std::string sql = "SELECT /*+LABEL(vfts_broker_serve)*/ served FROM (SELECT vfts.tserve(* USING PARAMETERS index=" + sql_lit(index) +
                          ", endpoint=" + sql_lit(cfg.socket_path) + ", token=" + sql_lit(token) + ", workers=" + std::to_string(cfg.workers) +
                          ", k=" + std::to_string(cfg.k);
        for (const std::string &p : cfg.params) sql += ", " + p;
        sql += ") OVER() FROM " + cfg.schema + "." + index + "_" + cfg.view + ") r;";
        // the statement runs until the channel closes; its output (or its error) comes at the end
        stmt_ = std::thread([this, sql] {
            std::vector<std::string> lines;
            s_->run(sql, lines);
            std::string err;
            for (const std::string &l : lines) if (l.find("ERROR") != std::string::npos) { err = l; break; }
            std::lock_guard<std::mutex> l(mu_);
            if (error_.empty()) error_ = err.empty() ? "the vfts.tserve statement ended" : err;
        });
        fd_ = acceptor.wait(token, 60);
        if (fd_ < 0) {
            acceptor.forget(token);
            stmt_.detach();
            std::lock_guard<std::mutex> l(mu_);
            throw std::runtime_error(error_.empty() ? "vfts.tserve did not connect" : error_);
        }
        alive_ = true;
        reader_ = std::thread([this] { read_loop(); });
    }
    ~Channel() { if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR); if (reader_.joinable()) reader_.join(); if (stmt_.joinable()) stmt_.join(); if (fd_ >= 0) ::close(fd_); }

    bool alive() const { return alive_; }
    std::string error() { std::lock_guard<std::mutex> l(mu_); return error_; }

    void submit(const std::shared_ptr<Pending> &p)
    {
        std::uint32_t id;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (!alive_) { p->finish(503, "{\"error\": \"" + json_escape(error_) + "\"}"); return; }
            id = next_++;
            waiting_[id] = p;
        }
        std::string f(8, '\0');
        const std::uint32_t len = static_cast<std::uint32_t>(4 + p->query.size());
        std::memcpy(&f[0], &len, 4);
        std::memcpy(&f[4], &id, 4);
        f += p->query;
        std::lock_guard<std::mutex> l(wmu_);
        std::size_t off = 0;
        while (off < f.size()) {
            const ssize_t w = ::send(fd_, f.data() + off, f.size() - off, MSG_NOSIGNAL);
            if (w <= 0) { ::shutdown(fd_, SHUT_RDWR); return; }      // the reader fails the waiting requests
            off += static_cast<std::size_t>(w);
        }
    }
};

class ServeLane : public Lane {
    std::string index_, user_, password_;
    std::mutex mu_;
    std::vector<std::unique_ptr<Channel>> ch_;
    std::atomic<std::uint64_t> rr_{0};
    std::string error_;

public:
    ServeLane(std::string index, std::string user, std::string password)
        : index_(std::move(index)), user_(std::move(user)), password_(std::move(password))
    {
        try { for (int i = 0; i < cfg.channels; ++i) ch_.emplace_back(new Channel(index_, user_, password_)); }
        catch (const std::exception &e) { error_ = e.what(); }
    }
    std::string error() override { std::lock_guard<std::mutex> l(mu_); return error_; }
    void submit(const std::shared_ptr<Pending> &p) override
    {
        Channel *c;
        {
            std::lock_guard<std::mutex> l(mu_);
            const std::size_t i = rr_++ % ch_.size();
            if (!ch_[i]->alive()) {     // the statement ended (canceled, node restart): a new one
                try { ch_[i].reset(new Channel(index_, user_, password_)); }
                catch (const std::exception &e) { p->finish(503, "{\"error\": \"" + json_escape(e.what()) + "\"}"); return; }
            }
            c = ch_[i].get();
        }
        c->submit(p);
    }
};

std::mutex lanes_mu;
std::map<std::string, std::unique_ptr<Lane>> lanes;     // key: user \x01 password \x01 index

Lane *lane_for(const std::string &user, const std::string &password, const std::string &index)
{
    const std::string key = user + '\x01' + password + '\x01' + index;
    std::lock_guard<std::mutex> l(lanes_mu);
    auto it = lanes.find(key);
    if (it == lanes.end()) {
        Lane *l = cfg.serve ? static_cast<Lane *>(new ServeLane(index, user, password)) : static_cast<Lane *>(new SqlLane(index, user, password));
        it = lanes.emplace(key, std::unique_ptr<Lane>(l)).first;
    }
    return it->second.get();
}

// ------------------------------------------------------------------ HTTP

void reply(int fd, int status, const std::string &body)
{
    const char *text = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" : status == 404 ? "Not Found" : status == 503 ? "Service Unavailable" : "Internal Server Error";
    std::string r = "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" + body;
    std::size_t off = 0;
    while (off < r.size()) {
        const ssize_t w = ::send(fd, r.data() + off, r.size() - off, MSG_NOSIGNAL);
        if (w <= 0) return;
        off += static_cast<std::size_t>(w);
    }
}

void connection(int fd)
{
    std::string buf;
    char tmp[65536];
    for (;;) {
        std::size_t he;
        while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t r = ::recv(fd, tmp, sizeof tmp, 0);
            if (r <= 0) { ::close(fd); return; }
            buf.append(tmp, static_cast<std::size_t>(r));
        }
        const std::string head = buf.substr(0, he);
        std::string lower = head;
        for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::size_t len = 0;
        const std::size_t cl = lower.find("content-length:");
        if (cl != std::string::npos) len = std::strtoull(lower.c_str() + cl + 15, nullptr, 10);
        while (buf.size() < he + 4 + len) {
            const ssize_t r = ::recv(fd, tmp, sizeof tmp, 0);
            if (r <= 0) { ::close(fd); return; }
            buf.append(tmp, static_cast<std::size_t>(r));
        }
        const std::string body = buf.substr(he + 4, len);
        buf.erase(0, he + 4 + len);
        // request line: POST /<index>/_search HTTP/1.1
        const std::size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
        const std::string path = sp1 == std::string::npos ? "" : head.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::size_t sl = path.find('/', 1);
        const std::string index = sl == std::string::npos ? "" : path.substr(1, sl - 1);
        const std::string action = sl == std::string::npos ? "" : path.substr(sl + 1);
        if (!safe_name(index) || (action != "_search" && action != "_count")) { reply(fd, 404, "{\"error\": \"POST /<index>/_search\"}"); continue; }
        if (action == "_count") { reply(fd, 200, "{\"count\": 0}"); continue; }      // connection check of the load generator
        std::string user, password;
        const std::size_t au = lower.find("\r\nauthorization: basic ");
        if (au != std::string::npos) {
            const std::size_t s = au + 23, e = head.find("\r\n", s);
            const std::string up = base64_decode(head.substr(s, e == std::string::npos ? std::string::npos : e - s));
            const std::size_t c = up.find(':');
            if (c == std::string::npos) { reply(fd, 401, "{\"error\": \"bad credentials\"}"); continue; }
            user = up.substr(0, c);
            password = up.substr(c + 1);
        }
        auto p = std::make_shared<Pending>();
        if (!json_string(body, "query", p->query)) { reply(fd, 400, "{\"error\": \"body: {\\\"query\\\": \\\"...\\\"}\"}"); continue; }
        Lane *lane = lane_for(user, password, index);
        const std::string err = lane->error();
        if (!err.empty()) { reply(fd, 401, "{\"error\": \"" + json_escape(err) + "\"}"); continue; }
        lane->submit(p);
        p->wait();
        reply(fd, p->status, p->body);
    }
}

const char *opt(int argc, char **argv, const char *name, const char *def)
{
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}

} // namespace

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    cfg.port = std::atoi(opt(argc, argv, "--port", "9300"));
    cfg.bind = opt(argc, argv, "--bind", "127.0.0.1");
    cfg.schema = opt(argc, argv, "--schema", "public");
    cfg.view = opt(argc, argv, "--view", "snap");
    cfg.sessions = std::max(1, std::atoi(opt(argc, argv, "--sessions", "4")));
    cfg.max_batch = std::max(1, std::atoi(opt(argc, argv, "--max-batch", "32")));
    cfg.k = std::max(1, std::atoi(opt(argc, argv, "--k", "10")));
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], "--param") == 0) cfg.params.push_back(argv[i + 1]);
    const std::string mode = opt(argc, argv, "--mode", "sql");
    if (mode != "sql" && mode != "serve") { std::fprintf(stderr, "--mode sql|serve\n"); return 2; }
    cfg.serve = mode == "serve";
    cfg.channels = std::max(1, std::atoi(opt(argc, argv, "--channels", "1")));
    cfg.workers = std::max(0, std::atoi(opt(argc, argv, "--workers", "0")));
    cfg.socket_path = opt(argc, argv, "--socket", ("/tmp/vfts_broker." + std::to_string(cfg.port) + ".sock").c_str());
    if (cfg.serve) acceptor.start();
    if (!safe_name(cfg.schema) || (cfg.view != "snap" && cfg.view != "live")) { std::fprintf(stderr, "bad --schema or --view\n"); return 2; }
    const int ls = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(cfg.port));
    ::inet_pton(AF_INET, cfg.bind.c_str(), &a.sin_addr);
    if (::bind(ls, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(ls, 256) != 0) { std::perror("bind"); return 1; }
    if (cfg.serve)
        std::fprintf(stderr, "vfts_broker on %s:%d, schema %s, view %s, mode serve: %d tserve statement(s) per user and index, channel %s\n",
                     cfg.bind.c_str(), cfg.port, cfg.schema.c_str(), cfg.view.c_str(), cfg.channels, cfg.socket_path.c_str());
    else
        std::fprintf(stderr, "vfts_broker on %s:%d, schema %s, view %s, mode sql: %d sessions per user and index, batches up to %d\n",
                     cfg.bind.c_str(), cfg.port, cfg.schema.c_str(), cfg.view.c_str(), cfg.sessions, cfg.max_batch);
    for (;;) {
        const int fd = ::accept4(ls, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        std::thread(connection, fd).detach();
    }
}
