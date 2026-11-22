// Document reordering by recursive graph bisection (Dhulipala et al., "Compressing Graphs and
// Indexes with Recursive Graph Bisection", KDD 2016; the method of Lucene's BPIndexReorderer and of
// PISA). Documents that share words get nearby docnos: postings get smaller gaps (fewer bits) and
// the documents that score high for a word cluster in fewer blocks, so block-max bounds skip more.
//
// The documents (a forward index: the selected terms of every document) are cut in two halves;
// documents are swapped between the halves while that lowers the cost of the log-gap model,
// sum over terms of d * log2(n / (d + 1)) per half (d: documents of the half with the term, n: the
// half's size); then both halves are cut again, down to min_partition documents. The result only
// changes how documents are numbered inside a segment: search results are the same (ties are
// broken by doc_id), checked by the engine tests and by comparing every hit of the benchmarks.
#ifndef VTEXT_ENGINE_REORDER_H
#define VTEXT_ENGINE_REORDER_H

#include <cstdint>
#include <vector>

namespace vtext {

struct ForwardIndex {
    std::uint32_t n_docs = 0;
    std::uint32_t n_terms = 0;
    std::vector<std::uint64_t> off;     // n_docs + 1: document d's terms are terms[off[d] .. off[d + 1])
    std::vector<std::uint32_t> terms;   // term numbers 0 .. n_terms - 1
};

struct ReorderOptions {
    std::uint32_t max_iters = 20;       // swap rounds per bisection (Lucene's default)
    std::uint32_t min_partition = 32;   // documents: smaller ranges are not cut again
    std::uint32_t min_df = 0;           // terms with fewer documents are left out (0: reorder_min_df)
    int threads = 0;                    // 0: every core
};

struct ReorderStats {
    std::uint64_t swaps = 0, rounds = 0;
    std::uint32_t levels = 0;
    double seconds = 0;
};

// The new order: order[i] is the document (forward index number) that gets docno i. The input
// order is the starting point; equal input gives equal output.
std::vector<std::uint32_t> bisect_order(const ForwardIndex &f, const ReorderOptions &o, ReorderStats *stats = nullptr);

// Terms with fewer documents than this are left out of the forward index of a segment of n
// documents (they cannot move many documents; Lucene leaves out terms below 4,096 documents).
inline std::uint32_t reorder_min_df(std::uint64_t n_docs)
{
    const std::uint64_t v = n_docs / 2048;
    return static_cast<std::uint32_t>(v < 2 ? 2 : v > 4096 ? 4096 : v);
}

} // namespace vtext

#endif
