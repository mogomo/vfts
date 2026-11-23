#include "cache.h"
#include "attribute.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vfts {

namespace {

constexpr char GEN_MAGIC[8] = {'V', 'F', 'T', 'S', 'G', 'E', 'N', '1'};

[[noreturn]] void sys_fail(const std::string &what) { throw std::runtime_error(what + ": " + std::strerror(errno)); }

void mkdirs(const std::string &path)
{
    std::string cur;
    std::size_t i = 0;
    while (i <= path.size()) {
        const std::size_t j = path.find('/', i);
        cur = path.substr(0, j == std::string::npos ? path.size() : j);
        if (!cur.empty() && ::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) sys_fail("cannot create directory " + cur);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) sys_fail("not a directory: " + path);
}

std::uint64_t fnv64(const void *p, std::size_t n)
{
    const unsigned char *b = static_cast<const unsigned char *>(p);
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

std::string seg_name(std::int64_t id) { return "seg_" + std::to_string(id) + ".vfs"; }
std::string gen_name(std::int64_t g) { return "gen_" + std::to_string(g) + ".vfg"; }

// Writes a file atomically: temporary name, fsync, rename, fsync of the directory.
void write_atomic(const std::string &dir, const std::string &name, const void *data, std::size_t n)
{
    static std::atomic<unsigned> counter{0};
    const std::string tmp = dir + "/" + name + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(counter++);
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) sys_fail("cannot create " + tmp);
    const char *p = static_cast<const char *>(data);
    std::size_t left = n;
    while (left) {
        const ssize_t w = ::write(fd, p, left);
        if (w < 0) { if (errno == EINTR) continue; const int e = errno; ::close(fd); ::unlink(tmp.c_str()); errno = e; sys_fail("cannot write " + tmp); }
        p += w;
        left -= static_cast<std::size_t>(w);
    }
    if (::fsync(fd) != 0) { const int e = errno; ::close(fd); ::unlink(tmp.c_str()); errno = e; sys_fail("cannot sync " + tmp); }
    ::close(fd);
    if (::rename(tmp.c_str(), (dir + "/" + name).c_str()) != 0) { const int e = errno; ::unlink(tmp.c_str()); errno = e; sys_fail("cannot rename " + tmp); }
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
}

bool read_small(const std::string &path, std::string &out)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid()) { ::close(fd); return false; }
    out.resize(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < out.size()) {
        const ssize_t r = ::read(fd, &out[got], out.size() - got);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        got += static_cast<std::size_t>(r);
    }
    ::close(fd);
    out.resize(got);
    return true;
}

// ---- mappings

struct Mapping {
    void *p = nullptr;
    std::size_t n = 0;
    dev_t dev = 0;
    ino_t ino = 0;
    ~Mapping() { if (p && n) ::munmap(p, n); }
};

std::shared_ptr<Mapping> map_file(const std::string &path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid()) { ::close(fd); return nullptr; }
    auto m = std::make_shared<Mapping>();
    m->n = static_cast<std::size_t>(st.st_size);
    m->dev = st.st_dev;
    m->ino = st.st_ino;
    if (m->n) {
        m->p = ::mmap(nullptr, m->n, PROT_READ, MAP_SHARED, fd, 0);
        if (m->p == MAP_FAILED) { m->p = nullptr; const int e = errno; ::close(fd); errno = e; sys_fail("cannot map " + path); }
        ::madvise(m->p, m->n, MADV_WILLNEED);
    }
    ::close(fd);
    return m;
}

// ---- generation file

struct GenRecord {
    std::int64_t segment_id;
    std::uint64_t seg_checksum;
    std::uint32_t n_dead, n_dead_terms, n_dead_stems, pad;
};
static_assert(sizeof(GenRecord) == 32, "GenRecord size");

struct GenHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t n_segs;
    std::int64_t gen;
    std::uint64_t checksum;     // fnv64 of the bytes after the header
    std::uint64_t total_bytes;
};
static_assert(sizeof(GenHeader) == 40, "GenHeader size");

struct GenData {
    std::int64_t gen = 0;
    std::string options;
    std::string access;
    std::vector<std::string> keys;
    struct Seg {
        std::int64_t id;
        std::uint64_t checksum;
        std::vector<std::uint32_t> dead;
        DeadCounts counts;
    };
    std::vector<Seg> segs;
};

void put(std::string &s, const void *p, std::size_t n) { s.append(static_cast<const char *>(p), n); }

std::string encode_gen(const GenData &g)
{
    std::string body;
    const std::uint32_t olen = static_cast<std::uint32_t>(g.options.size());
    put(body, &olen, 4);
    body += g.options;
    const std::uint32_t alen = static_cast<std::uint32_t>(g.access.size()), nk = static_cast<std::uint32_t>(g.keys.size());
    put(body, &alen, 4);
    body += g.access;
    put(body, &nk, 4);
    for (const std::string &k : g.keys) {
        const std::uint32_t kl = static_cast<std::uint32_t>(k.size());
        put(body, &kl, 4);
        body += k;
    }
    while (body.size() % 8) body.push_back('\0');
    for (const auto &s : g.segs) {
        GenRecord r{s.id, s.checksum, static_cast<std::uint32_t>(s.dead.size()),
                    static_cast<std::uint32_t>(s.counts.terms.size()), static_cast<std::uint32_t>(s.counts.stems.size()), 0};
        put(body, &r, sizeof(r));
        if (!s.dead.empty()) put(body, s.dead.data(), 4 * s.dead.size());
        for (auto &p : s.counts.terms) { put(body, &p.first, 4); put(body, &p.second, 4); }
        for (auto &p : s.counts.stems) { put(body, &p.first, 4); put(body, &p.second, 4); }
        while (body.size() % 8) body.push_back('\0');
    }
    GenHeader h{};
    std::memcpy(h.magic, GEN_MAGIC, 8);
    h.version = 2;
    h.n_segs = static_cast<std::uint32_t>(g.segs.size());
    h.gen = g.gen;
    h.checksum = fnv64(body.data(), body.size());
    h.total_bytes = sizeof(h) + body.size();
    std::string out;
    put(out, &h, sizeof(h));
    out += body;
    return out;
}

bool decode_gen(const std::string &bytes, GenData &g, std::string &why)
{
    if (bytes.size() < sizeof(GenHeader)) { why = "generation file too short"; return false; }
    GenHeader h;
    std::memcpy(&h, bytes.data(), sizeof(h));
    if (std::memcmp(h.magic, GEN_MAGIC, 8) != 0 || (h.version != 1 && h.version != 2)) { why = "not a generation file"; return false; }
    if (h.total_bytes != bytes.size()) { why = "generation file has the wrong size"; return false; }
    if (fnv64(bytes.data() + sizeof(h), bytes.size() - sizeof(h)) != h.checksum) { why = "generation file checksum mismatch"; return false; }
    g.gen = h.gen;
    g.segs.clear();
    std::size_t at = sizeof(h);
    std::uint32_t olen = 0;
    if (at + 4 > bytes.size()) { why = "generation file truncated"; return false; }
    std::memcpy(&olen, bytes.data() + at, 4);
    at += 4;
    if (at + olen > bytes.size()) { why = "generation file truncated"; return false; }
    g.options.assign(bytes.data() + at, olen);
    at += olen;
    g.access.clear();
    g.keys.clear();
    if (h.version >= 2) {
        // access mode and keys (version 1, written by vfts 0.1.0, had none)
        auto str = [&](std::string &out) {
            std::uint32_t n = 0;
            if (at + 4 > bytes.size()) return false;
            std::memcpy(&n, bytes.data() + at, 4);
            at += 4;
            if (at + n > bytes.size()) return false;
            out.assign(bytes.data() + at, n);
            at += n;
            return true;
        };
        std::uint32_t nk = 0;
        if (!str(g.access) || at + 4 > bytes.size()) { why = "generation file truncated"; return false; }
        std::memcpy(&nk, bytes.data() + at, 4);
        at += 4;
        if (nk > 16) { why = "generation file damaged"; return false; }
        g.keys.resize(nk);
        for (auto &k : g.keys) if (!str(k)) { why = "generation file truncated"; return false; }
    }
    while (at % 8) ++at;
    for (std::uint32_t i = 0; i < h.n_segs; ++i) {
        if (at + sizeof(GenRecord) > bytes.size()) { why = "generation file truncated"; return false; }
        GenRecord r;
        std::memcpy(&r, bytes.data() + at, sizeof(r));
        at += sizeof(r);
        const std::size_t need = 4ull * r.n_dead + 8ull * r.n_dead_terms + 8ull * r.n_dead_stems;
        if (at + need > bytes.size()) { why = "generation file truncated"; return false; }
        GenData::Seg s;
        s.id = r.segment_id;
        s.checksum = r.seg_checksum;
        s.dead.resize(r.n_dead);
        if (r.n_dead) std::memcpy(s.dead.data(), bytes.data() + at, 4ull * r.n_dead);
        at += 4ull * r.n_dead;
        s.counts.terms.resize(r.n_dead_terms);
        for (auto &p : s.counts.terms) { std::memcpy(&p.first, bytes.data() + at, 4); std::memcpy(&p.second, bytes.data() + at + 4, 4); at += 8; }
        s.counts.stems.resize(r.n_dead_stems);
        for (auto &p : s.counts.stems) { std::memcpy(&p.first, bytes.data() + at, 4); std::memcpy(&p.second, bytes.data() + at + 4, 4); at += 8; }
        while (at % 8) ++at;
        g.segs.push_back(std::move(s));
    }
    return true;
}

bool read_active(const std::string &dir, std::int64_t &gen)
{
    std::string s;
    if (!read_small(dir + "/ACTIVE", s)) return false;
    char *end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno || end == s.c_str()) return false;
    gen = v;
    return true;
}

// ---- the process-wide registry

struct Registry {
    std::mutex mu;
    struct Entry {
        std::shared_ptr<const LoadedIndex> idx;
        std::chrono::steady_clock::time_point checked;
        dev_t dev = 0;                  // the generation file it was read from: a new activation of
        ino_t ino = 0;                  // the same generation (new keys) replaces the file
    };
    std::map<std::string, Entry> indexes;                       // key: dir
    std::map<std::string, std::weak_ptr<Mapping>> mappings;     // key: path
};

Registry &registry()
{
    static Registry *r = new Registry();        // never destroyed: queries may run during exit
    return *r;
}

std::shared_ptr<Mapping> shared_mapping(Registry &r, const std::string &path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return nullptr;
    auto it = r.mappings.find(path);
    if (it != r.mappings.end()) {
        auto m = it->second.lock();
        if (m && m->dev == st.st_dev && m->ino == st.st_ino && m->n == static_cast<std::size_t>(st.st_size)) return m;
    }
    auto m = map_file(path);
    if (m) r.mappings[path] = m;
    return m;
}

std::shared_ptr<const LoadedIndex> load_generation(Registry &r, const std::string &dir, std::int64_t gen, const std::string &index)
{
    std::string bytes, why;
    if (!read_small(dir + "/" + gen_name(gen), bytes))
        throw std::runtime_error("cache of index " + index + ": generation file " + gen_name(gen) + " is missing: run CALL vfts.load_all('" + index + "')");
    GenData g;
    if (!decode_gen(bytes, g, why)) throw std::runtime_error("cache of index " + index + ": " + why + ": run CALL vfts.load_all('" + index + "')");
    auto li = std::make_shared<LoadedIndex>();
    li->gen = g.gen;
    li->options = g.options;
    li->access = g.access;
    li->keys = g.keys;
    for (auto &s : g.segs) {
        auto m = shared_mapping(r, dir + "/" + seg_name(s.id));
        if (!m) throw std::runtime_error("cache of index " + index + ": segment file " + seg_name(s.id) + " is missing: run CALL vfts.load_all('" + index + "')");
        auto seg = std::make_shared<Segment>(m->p, m->n);
        if (seg->header().checksum != s.checksum)
            throw std::runtime_error("cache of index " + index + ": segment file " + seg_name(s.id) + " is not the one of generation " + std::to_string(gen) + ": run CALL vfts.load_all('" + index + "')");
        SegmentRef ref;
        ref.seg = seg.get();
        if (!s.dead.empty()) {
            auto d = std::make_shared<DocSet>();
            d->resize(seg->n_docs());
            for (std::uint32_t x : s.dead) if (x < seg->n_docs()) d->set(x);
            ref.dead = d;
            ref.dead_counts = std::make_shared<DeadCounts>(std::move(s.counts));
        }
        li->refs.push_back(ref);
        li->bytes += m->n;
        li->docs += seg->n_docs();
        li->dead += s.dead.size();
        li->keep.push_back(m);
        li->keep.push_back(seg);
    }
    return li;
}

} // namespace

bool valid_index_name(const std::string &name)
{
    if (name.empty() || name.size() > 64) return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

static std::string index_dir(const std::string &cache_dir, const std::string &index)
{
    if (!valid_index_name(index)) throw std::runtime_error("index name '" + index + "' is not valid: use letters, digits and underscore");
    if (cache_dir.empty() || cache_dir[0] != '/') throw std::runtime_error("cache_dir must be an absolute path: '" + cache_dir + "'");
    return cache_dir + "/" + index;
}

CacheWriter::CacheWriter(const std::string &cache_dir, const std::string &index) : dir_(index_dir(cache_dir, index))
{
    mkdirs(dir_);
}

bool CacheWriter::has_segment(std::int64_t id, std::uint64_t checksum) const
{
    const int fd = ::open((dir_ + "/" + seg_name(id)).c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    SegmentHeader h;
    struct stat st;
    const bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && ::pread(fd, &h, sizeof(h), 0) == static_cast<ssize_t>(sizeof(h)) &&
                    std::memcmp(h.magic, SEGMENT_MAGIC, 8) == 0 && h.checksum == checksum &&
                    h.total_bytes == static_cast<std::uint64_t>(st.st_size) && h.segment_id == id;
    ::close(fd);
    return ok;
}

void CacheWriter::write_segment(const std::string &bytes)
{
    Segment seg(bytes.data(), bytes.size());
    seg.verify();
    write_atomic(dir_, seg_name(seg.id()), bytes.data(), bytes.size());
}

void CacheWriter::activate(const Generation &g)
{
    std::int64_t prev_gen = -1;
    GenData prev;
    {
        std::string bytes, why;
        if (read_active(dir_, prev_gen) && read_small(dir_ + "/" + gen_name(prev_gen), bytes)) decode_gen(bytes, prev, why);
        else prev_gen = -1;
    }
    GenData out;
    out.gen = g.gen;
    out.options = g.options;
    out.access = g.access;
    out.keys = g.keys;
    for (const GenSegment &s : g.segs) {
        if (!has_segment(s.segment_id, s.checksum))
            throw std::runtime_error("segment " + std::to_string(s.segment_id) + " is not in the cache of this node (or differs)");
        GenData::Seg rec;
        rec.id = s.segment_id;
        rec.checksum = s.checksum;
        rec.dead = s.dead;
        std::sort(rec.dead.begin(), rec.dead.end());
        rec.dead.erase(std::unique(rec.dead.begin(), rec.dead.end()), rec.dead.end());
        if (!rec.dead.empty()) {
            bool reused = false;
            for (auto &p : prev.segs)
                if (p.id == rec.id && p.checksum == rec.checksum && p.dead == rec.dead) { rec.counts = p.counts; reused = true; break; }
            if (!reused) {
                auto m = map_file(dir_ + "/" + seg_name(s.segment_id));
                if (!m) sys_fail("cannot open segment " + std::to_string(s.segment_id));
                Segment seg(m->p, m->n);
                DocSet d;
                d.resize(seg.n_docs());
                for (std::uint32_t x : rec.dead) {
                    if (x >= seg.n_docs()) throw std::runtime_error("tombstone docno " + std::to_string(x) + " out of range in segment " + std::to_string(rec.id));
                    d.set(x);
                }
                rec.counts = compute_dead_counts(seg, d);
            }
        }
        out.segs.push_back(std::move(rec));
    }
    const std::string bytes = encode_gen(out);
    write_atomic(dir_, gen_name(g.gen), bytes.data(), bytes.size());
    const std::string active = std::to_string(g.gen) + "\n";
    write_atomic(dir_, "ACTIVE", active.data(), active.size());

    // keep the files of this and the previous generation; remove other vfts files
    std::vector<std::string> keep = {"ACTIVE", gen_name(g.gen)};
    for (auto &s : out.segs) keep.push_back(seg_name(s.id));
    if (prev_gen >= 0 && prev_gen != g.gen) {
        keep.push_back(gen_name(prev_gen));
        for (auto &s : prev.segs) keep.push_back(seg_name(s.id));
    }
    DIR *d = ::opendir(dir_.c_str());
    if (!d) return;
    while (dirent *e = ::readdir(d)) {
        const std::string n = e->d_name;
        const bool ours = (n.rfind("seg_", 0) == 0 && n.find(".vfs") != std::string::npos) ||
                          (n.rfind("gen_", 0) == 0 && n.find(".vfg") != std::string::npos);
        if (!ours || std::find(keep.begin(), keep.end(), n) != keep.end()) continue;
        ::unlink((dir_ + "/" + n).c_str());
    }
    ::closedir(d);
}

static bool gen_file_id(const std::string &dir, std::int64_t gen, dev_t &dev, ino_t &ino)
{
    struct stat st;
    if (::stat((dir + "/gen_" + std::to_string(gen) + ".vfg").c_str(), &st) != 0) return false;
    dev = st.st_dev;
    ino = st.st_ino;
    return true;
}

void CacheWriter::write_attribute(const std::string &name, const std::string &bytes)
{
    if (!valid_index_name(name)) throw std::runtime_error("invalid attribute name '" + name + "'");
    Attribute check(bytes.data(), bytes.size());
    write_atomic(dir_, attr_file_name(name), bytes.data(), bytes.size());
}

void CacheWriter::remove_attribute(const std::string &name)
{
    if (!valid_index_name(name)) throw std::runtime_error("invalid attribute name '" + name + "'");
    ::unlink((dir_ + "/" + attr_file_name(name)).c_str());
}

std::shared_ptr<const LoadedIndex> open_index(const std::string &cache_dir, const std::string &index, std::int64_t min_gen, bool exact)
{
    const std::string dir = index_dir(cache_dir, index);
    Registry &r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    const auto now = std::chrono::steady_clock::now();
    auto it = r.indexes.find(dir);
    if (it != r.indexes.end() && (exact ? it->second.idx->gen == min_gen : it->second.idx->gen >= min_gen) &&
        now - it->second.checked < std::chrono::milliseconds(ACTIVE_CHECK_MS))
        return it->second.idx;
    std::int64_t gen;
    if (!read_active(dir, gen))
        throw std::runtime_error("no cache for index " + index + " on this node (" + dir + "): run CALL vfts.load_all('" + index + "')");
    if (gen < min_gen)
        throw std::runtime_error("cache of index " + index + " is stale on this node (generation " + std::to_string(gen) +
                                 ", the view needs " + std::to_string(min_gen) + "): run CALL vfts.load_all('" + index + "')");
    if (exact && gen != min_gen) {
        // a newer generation is active, the view still belongs to the previous one (a refresh is
        // switching): open that one, not kept in the registry
        dev_t dev;
        ino_t ino;
        if (!gen_file_id(dir, min_gen, dev, ino))
            throw std::runtime_error("index " + index + " changed while this statement started (generation " + std::to_string(min_gen) +
                                     " is gone on this node): run the statement again");
        return load_generation(r, dir, min_gen, index);
    }
    dev_t dev = 0;
    ino_t ino = 0;
    gen_file_id(dir, gen, dev, ino);
    if (it != r.indexes.end() && it->second.idx->gen == gen && it->second.dev == dev && it->second.ino == ino) {
        it->second.checked = now;
        return it->second.idx;
    }
    auto li = load_generation(r, dir, gen, index);
    r.indexes[dir] = {li, now, dev, ino};
    // forget generations that are no longer active (an index dropped or rebuilt, or another
    // generation activated by a statement in another process): their segment files may be
    // deleted, and a mapping kept here would hold their memory (a tmpfs cache) until a restart
    for (auto e = r.indexes.begin(); e != r.indexes.end();) {
        std::int64_t g2;
        if (e->first != dir && (!read_active(e->first, g2) || g2 != e->second.idx->gen)) e = r.indexes.erase(e);
        else ++e;
    }
    // forget mappings that nobody uses any more
    for (auto m = r.mappings.begin(); m != r.mappings.end();) m = m->second.expired() ? r.mappings.erase(m) : std::next(m);
    return li;
}

CacheInfo cache_info(const std::string &cache_dir, const std::string &index)
{
    CacheInfo ci;
    try {
        auto li = open_index(cache_dir, index, 0);
        ci.present = true;
        ci.gen = li->gen;
        ci.segments = li->refs.size();
        ci.bytes = li->bytes;
        ci.docs = li->docs;
        ci.dead = li->dead;
    } catch (const std::exception &e) {
        ci.error = e.what();
    }
    return ci;
}

void drop_cache(const std::string &cache_dir, const std::string &index)
{
    const std::string dir = index_dir(cache_dir, index);
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mu);
        r.indexes.erase(dir);
    }
    forget_attributes(cache_dir, index);
    DIR *d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent *e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n == "ACTIVE" || n.rfind("seg_", 0) == 0 || n.rfind("gen_", 0) == 0 || n.rfind("attr_", 0) == 0) ::unlink((dir + "/" + n).c_str());
    }
    ::closedir(d);
    ::rmdir(dir.c_str());
}

} // namespace vfts
