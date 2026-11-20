// Segment format: builder -> bytes -> reader, against a brute-force model of the documents;
// table rows round trip to identical bytes; norms; union df of stem groups; bitmaps.
#include "check.h"
#include "../../src/engine/builder.h"
#include "../../src/engine/postings.h"
#include "../../src/engine/segment.h"
#include "../../src/engine/table_codec.h"

#include <roaring.h>

#include <algorithm>
#include <map>
#include <set>

using namespace vtext;

static void norms()
{
    // Lucene SmallFloat.intToByte4 values (checked against Lucene 9.x)
    CHECK_EQ(int(length_to_norm(0)), 0);
    CHECK_EQ(int(length_to_norm(23)), 23);
    CHECK_EQ(int(length_to_norm(24)), 24);
    CHECK_EQ(norm_to_length(length_to_norm(24)), 24u);
    CHECK_EQ(norm_to_length(length_to_norm(100)), 96u);
    CHECK_EQ(norm_to_length(length_to_norm(1000)), 984u);
    CHECK_EQ(int(length_to_norm(0x7FFFFFFF)), 255);
    for (std::uint32_t len = 0; len < 100000; len += 7) {
        const std::uint32_t q = norm_to_length(length_to_norm(len));
        CHECK(q <= len);
        if (len > 0) CHECK(length_to_norm(len) >= length_to_norm(len - 1));
    }
}

static void bits()
{
    Rng rng(5);
    for (std::uint32_t b = 0; b <= 32; ++b) {
        std::uint32_t v[BLOCK], w[BLOCK];
        for (std::uint32_t i = 0; i < BLOCK; ++i) v[i] = b == 32 ? static_cast<std::uint32_t>(rng.next()) : static_cast<std::uint32_t>(rng.next() & ((1ull << b) - 1));
        std::string s;
        pack_bits(s, v, BLOCK, b);
        s.append(8, '\0');
        unpack_bits(reinterpret_cast<const std::uint8_t *>(s.data()), BLOCK, b, w);
        CHECK(std::equal(v, v + BLOCK, w));
    }
}

// A random corpus over a small vocabulary, with a brute-force model.
struct Model {
    std::map<std::int64_t, std::vector<std::string>> docs;    // doc_id -> words (already normalized)
};

static std::string word(std::uint64_t i) { static const char *w[] = {"run", "runs", "running", "ran", "contract", "contracts", "the", "of", "indemnity", "holmes", "watson", "x"}; return i < 12 ? w[i] : "w" + std::to_string(i); }

static Model make_model(std::uint64_t seed, int n_docs, int vocab, int max_len, std::int64_t id_step)
{
    Model m;
    Rng rng(seed);
    std::set<std::int64_t> used;
    for (int d = 0; d < n_docs; ++d) {
        std::int64_t id;
        do { id = rng.below(n_docs * id_step * 4) - n_docs * id_step; } while (!used.insert(id).second);
        auto &w = m.docs[id];
        const int len = static_cast<int>(rng.below(max_len + 1));
        for (int k = 0; k < len; ++k) w.push_back(word(static_cast<std::uint64_t>(rng.below(rng.below(4) == 0 ? 12 : vocab))));
    }
    return m;
}

static std::string build(const Model &m, std::uint64_t seed, std::int64_t segment_id)
{
    AnalyzerOptions o;
    o.default_lang = parse_lang("en");
    SegmentBuilder b(o);
    // shuffled arrival order
    std::vector<const std::pair<const std::int64_t, std::vector<std::string>> *> order;
    for (auto &kv : m.docs) order.push_back(&kv);
    Rng rng(seed);
    for (std::size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[rng.below(static_cast<std::int64_t>(i))]);
    for (auto *kv : order) {
        std::string text;
        for (auto &w : kv->second) text += w + " ";
        b.add(kv->first, text, LANG_NONE, kv->first);
    }
    return b.finish(segment_id);
}

static void check_against_model(const Model &m, const Segment &seg)
{
    seg.verify();
    CHECK_EQ(seg.n_docs(), static_cast<std::uint32_t>(m.docs.size()));
    // term -> doc_id -> positions
    std::map<std::string, std::map<std::int64_t, std::vector<std::uint32_t>>> inv;
    std::uint64_t sum = 0;
    for (auto &kv : m.docs) {
        for (std::uint32_t p = 0; p < kv.second.size(); ++p) inv[kv.second[p]][kv.first].push_back(p);
        sum += kv.second.size();
    }
    CHECK_EQ(seg.sum_len(), sum);
    CHECK_EQ(seg.n_terms(), static_cast<std::uint32_t>(inv.size()));
    std::uint32_t d = 0;
    for (auto &kv : m.docs) {
        CHECK_EQ(seg.doc_id(d), kv.first);
        CHECK_EQ(seg.doc_len(d), static_cast<std::uint32_t>(kv.second.size()));
        CHECK_EQ(seg.find_doc(kv.first), static_cast<std::int64_t>(d));
        ++d;
    }
    CHECK_EQ(seg.find_doc(m.docs.rbegin()->first + 1), std::int64_t(-1));
    std::uint32_t t = 0;
    std::vector<std::uint32_t> pos;
    for (auto &kv : inv) {
        CHECK_EQ(std::string(seg.term(t)), kv.first);
        CHECK_EQ(seg.find_term(kv.first), static_cast<std::int64_t>(t));
        CHECK_EQ(seg.info(t).df, static_cast<std::uint32_t>(kv.second.size()));
        PostingIter it(seg, t);
        auto e = kv.second.begin();
        for (std::uint32_t doc = it.next(); doc != NO_DOC; doc = it.next(), ++e) {
            if (e == kv.second.end()) { CHECK(false); break; }
            CHECK_EQ(seg.doc_id(doc), e->first);
            CHECK_EQ(it.tf(), static_cast<std::uint32_t>(e->second.size()));
            if ((doc & 3) == 0) {            // positions for some documents only: tests skipping
                pos.clear();
                it.positions(pos);
                CHECK(pos == e->second);
            }
        }
        CHECK(e == kv.second.end());
        // advance() to every 7th document and to targets between documents
        PostingIter a(seg, t);
        for (auto it2 = kv.second.begin(); it2 != kv.second.end(); ++it2) {
            const std::int64_t want = seg.find_doc(it2->first);
            if (want % 7 != 0) continue;
            CHECK_EQ(a.advance(static_cast<std::uint32_t>(want)), static_cast<std::uint32_t>(want));
            CHECK_EQ(a.tf(), static_cast<std::uint32_t>(it2->second.size()));
            pos.clear();
            a.positions(pos);
            CHECK(pos == it2->second);
        }
        CHECK_EQ(a.advance(seg.n_docs()), NO_DOC);
        // bitmap for frequent terms
        std::size_t blen;
        const std::uint8_t *bm = seg.bitmap(t, blen);
        CHECK_EQ(bm != nullptr, seg.info(t).df >= BITMAP_MIN_DF);
        if (bm) {
            roaring_bitmap_t *r = roaring_bitmap_portable_deserialize_safe(reinterpret_cast<const char *>(bm), blen);
            CHECK(r && roaring_bitmap_get_cardinality(r) == seg.info(t).df);
            if (r) roaring_bitmap_free(r);
        }
        ++t;
    }
    // stem groups: run, runs, running share the English stem "run"; union df is the union
    const std::int64_t s = seg.find_stem(stem_key("en", "run"));
    if (inv.count("run") || inv.count("runs") || inv.count("running")) {
        CHECK(s >= 0);
        std::set<std::int64_t> u;
        for (const char *w : {"run", "runs", "running"}) if (inv.count(w)) for (auto &kv : inv[w]) u.insert(kv.first);
        if (s >= 0) CHECK_EQ(seg.stem_union_df(static_cast<std::uint32_t>(s)), static_cast<std::uint32_t>(u.size()));
    }
}

static void roundtrip(const std::string &bytes)
{
    Segment seg(bytes.data(), bytes.size());
    SegmentAssembler as;
    std::size_t terms = 0, posts = 0, docs = 0;
    RowSink sink;
    // postings and positions are separate tables: feed them as separate rows, in reverse order
    std::vector<PostingRow> rows;
    sink.term = [&](const TermDictRow &r) { as.add(r); ++terms; };
    sink.posting = [&](const PostingRow &r) { rows.push_back(r); ++posts; };
    sink.doc = [&](const DocRow &r) { as.add(r); ++docs; };
    segment_to_rows(seg, sink);
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        PostingRow a = *it, b = *it;
        a.payload.clear();
        b.docs.clear();
        as.add(std::move(b));
        as.add(std::move(a));
        CHECK(it->docs.size() < 65000);       // fits a VARBINARY
    }
    const std::string again = as.finish(seg.id(), seg.header().max_ver);
    CHECK_EQ(again.size(), bytes.size());
    CHECK(again == bytes);
    CHECK(docs == seg.n_docs() && terms >= seg.n_terms() && posts >= seg.n_terms());
}

static void corrupt(const std::string &bytes)
{
    std::string b = bytes;
    b[b.size() / 2] ^= 0x55;
    CHECK(throws([&] { Segment s(b.data(), b.size()); s.verify(); }, "damaged"));
    CHECK(throws([&] { Segment s(bytes.data(), bytes.size() - 8); }, "damaged"));
    std::string m = bytes;
    m[0] = 'X';
    CHECK(throws([&] { Segment s(m.data(), m.size()); }, "magic"));
}

int main()
{
    norms();
    bits();
    {   // small, then large enough for bitmaps and several 65,536-docno chunks
        const Model m = make_model(1, 300, 50, 30, 3);
        const std::string bytes = build(m, 2, 7);
        Segment seg(bytes.data(), bytes.size());
        CHECK_EQ(seg.id(), std::int64_t(7));
        check_against_model(m, seg);
        roundtrip(bytes);
        corrupt(bytes);
        CHECK(build(m, 3, 7) == bytes);     // arrival order does not matter
    }
    {
        const Model m = make_model(11, 140000, 400, 12, 1);
        const std::string bytes = build(m, 12, 8);
        Segment seg(bytes.data(), bytes.size());
        check_against_model(m, seg);
        roundtrip(bytes);
        std::printf("140000 docs: %zu bytes, %u terms\n", bytes.size(), seg.n_terms());
    }
    {   // empty segment and empty documents
        Model m;
        m.docs[5] = {};
        m.docs[9] = {"x"};
        const std::string bytes = build(m, 1, 1);
        Segment seg(bytes.data(), bytes.size());
        check_against_model(m, seg);
        CHECK_EQ(seg.header().n_nonempty, 1u);
        roundtrip(bytes);
    }
    {   // duplicate doc id
        AnalyzerOptions o;
        SegmentBuilder b(o);
        b.add(1, "a");
        b.add(1, "b");
        CHECK(throws([&] { b.finish(1); }, "duplicate doc_id 1"));
    }
    return finish("test_segment");
}
