// httpload: a closed-loop HTTP load generator (C concurrent clients, one request at a time each,
// persistent keep-alive connections). Used to measure Elasticsearch and OpenSearch with a native
// client, so that the client is not what limits their queries per second.
//
//   httpload <port> <requests.ndjson> [--clients C] [--warmup W] [--repeat R] [--host 127.0.0.1]
//            [--times FILE]
//
// requests.ndjson: one request per line, "<path>\t<json body>" (POST, application/json). For
// _msearch the body's lines are separated by the byte 0x1e; they are sent as application/x-ndjson. Prints one JSON
// line: q/s of the best run, client ms percentiles (best time per request over the runs), errors.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

const char *opt(int argc, char **argv, const char *name, const char *def)
{
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}

struct Req { std::string wire; };

class Conn {
    int fd_ = -1;
    std::string host_;
    int port_;
    std::string buf_;

    bool open()
    {
        close();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        timeval tv{120, 0};     // never wait for ever: a request without an answer in 120 s is an error
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<std::uint16_t>(port_));
        ::inet_pton(AF_INET, host_.c_str(), &a.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) { close(); return false; }
        buf_.clear();
        return true;
    }
    void close() { if (fd_ >= 0) ::close(fd_); fd_ = -1; }

    // one response; returns the status (0: connection failed)
    int read_response()
    {
        std::size_t hdr_end;
        char tmp[65536];
        while ((hdr_end = buf_.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t r = ::recv(fd_, tmp, sizeof tmp, 0);
            if (r <= 0) return 0;
            buf_.append(tmp, static_cast<std::size_t>(r));
        }
        const int status = std::atoi(buf_.c_str() + 9);
        std::size_t len = 0;
        bool chunked = false;
        {
            std::string h = buf_.substr(0, hdr_end);
            for (char &c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const std::size_t cl = h.find("content-length:");
            if (cl != std::string::npos) len = std::strtoull(h.c_str() + cl + 15, nullptr, 10);
            chunked = h.find("transfer-encoding: chunked") != std::string::npos;
        }
        std::size_t at = hdr_end + 4;
        if (!chunked) {
            while (buf_.size() < at + len) {
                const ssize_t r = ::recv(fd_, tmp, sizeof tmp, 0);
                if (r <= 0) return 0;
                buf_.append(tmp, static_cast<std::size_t>(r));
            }
            buf_.erase(0, at + len);
            return status;
        }
        for (;;) {   // chunked body
            std::size_t le;
            while ((le = buf_.find("\r\n", at)) == std::string::npos) {
                const ssize_t r = ::recv(fd_, tmp, sizeof tmp, 0);
                if (r <= 0) return 0;
                buf_.append(tmp, static_cast<std::size_t>(r));
            }
            const std::size_t n = std::strtoull(buf_.c_str() + at, nullptr, 16);
            const std::size_t need = le + 2 + n + 2;
            while (buf_.size() < need) {
                const ssize_t r = ::recv(fd_, tmp, sizeof tmp, 0);
                if (r <= 0) return 0;
                buf_.append(tmp, static_cast<std::size_t>(r));
            }
            at = need;
            if (n == 0) break;
        }
        buf_.erase(0, at);
        return status;
    }

public:
    Conn(std::string host, int port) : host_(std::move(host)), port_(port) {}
    ~Conn() { close(); }

    int send(const std::string &wire)
    {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (fd_ < 0 && !open()) continue;
            std::size_t off = 0;
            bool ok = true;
            while (off < wire.size()) {
                const ssize_t w = ::send(fd_, wire.data() + off, wire.size() - off, MSG_NOSIGNAL);
                if (w <= 0) { ok = false; break; }
                off += static_cast<std::size_t>(w);
            }
            if (ok) { const int st = read_response(); if (st) return st; }
            close();
        }
        return 0;
    }
};

std::string unescape_newlines(std::string s)
{
    for (char &c : s) if (c == '\x1e') c = '\n';
    return s;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: httpload <port> <requests.ndjson> [--clients C] [--warmup W] [--repeat R]\n"); return 2; }
    const int port = std::atoi(argv[1]);
    const std::string host = opt(argc, argv, "--host", "127.0.0.1");
    const int clients = std::atoi(opt(argc, argv, "--clients", "32"));
    const int warmup = std::atoi(opt(argc, argv, "--warmup", "1"));
    const int repeat = std::atoi(opt(argc, argv, "--repeat", "3"));
    const char *times = opt(argc, argv, "--times", nullptr);
    std::vector<Req> reqs;
    {
        std::ifstream f(argv[2]);
        std::string line;
        while (std::getline(f, line)) {
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            const std::string path = line.substr(0, tab);
            std::string body = line.substr(tab + 1);
            const bool nd = path.find("_msearch") != std::string::npos;
            if (nd) body = unescape_newlines(body);
            Req r;
            r.wire = "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: " +
                     (nd ? "application/x-ndjson" : "application/json") + "\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: keep-alive\r\n\r\n" + body;
            reqs.push_back(std::move(r));
        }
    }
    const std::size_t n = reqs.size();
    std::vector<double> best(n, 1e18);
    double best_qps = 0;
    std::atomic<std::size_t> errors{0};
    for (int run = 0; run < warmup + repeat; ++run) {
        const bool measured = run >= warmup;
        std::atomic<std::size_t> next{0};
        std::vector<Conn *> conns;
        for (int c = 0; c < clients; ++c) {
            conns.push_back(new Conn(host, port));
            conns.back()->send(reqs.empty() ? std::string() : reqs[0].wire);   // connection set up, not measured
        }
        std::atomic<std::size_t> run_errors{0};
        const auto t0 = Clock::now();
        std::vector<std::thread> th;
        for (int c = 0; c < clients; ++c)
            th.emplace_back([&, c] {
                for (std::size_t i; (i = next++) < n;) {
                    const auto q0 = Clock::now();
                    const int st = conns[c]->send(reqs[i].wire);
                    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - q0).count();
                    if (st < 200 || st >= 300) { ++run_errors; continue; }
                    if (measured && ms < best[i]) best[i] = ms;   // one writer per i in a run
                }
            });
        for (auto &t : th) t.join();
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        for (Conn *c : conns) delete c;
        if (measured) { best_qps = std::max(best_qps, static_cast<double>(n) / secs); errors += run_errors.load(); }
    }
    if (times) {
        std::FILE *f = std::fopen(times, "w");
        for (std::size_t i = 0; i < n; ++i) if (best[i] < 1e17) std::fprintf(f, "%zu\t%.3f\n", i, best[i]);
        std::fclose(f);
    }
    std::vector<double> v;
    for (double b : best) if (b < 1e17) v.push_back(b);
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    auto pct = [&](double p) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())))]; };
    std::printf("{\"client\": \"httpload\", \"port\": %d, \"requests\": %zu, \"clients\": %d, \"errors\": %zu, "
                "\"client_ms\": {\"mean\": %.3f, \"p50\": %.3f, \"p90\": %.3f, \"p99\": %.3f, \"max\": %.3f}, \"qps_best\": %.1f}\n",
                port, n, clients, errors.load() / static_cast<std::size_t>(std::max(repeat, 1)), v.empty() ? 0 : sum / static_cast<double>(v.size()),
                pct(0.5), pct(0.9), pct(0.99), v.empty() ? 0 : v.back(), best_qps);
    return 0;
}
