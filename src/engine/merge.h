// Merging segments: the live documents of several segments (tombstoned ones dropped) into one new
// segment, without analyzing any text again. Postings and positions are merged term by term.
#ifndef VFTS_ENGINE_MERGE_H
#define VFTS_ENGINE_MERGE_H

#include "reorder.h"
#include "search.h"

#include <string>
#include <vector>

namespace vfts {

struct MergeOptions {
    // Number the documents by recursive graph bisection (reorder.h) instead of doc_id order.
    bool reorder = true;
    ReorderOptions bisection;
    ReorderStats *stats = nullptr;      // filled when set
};

// The new segment's bytes. Every doc_id must be live in at most one input segment.
std::string merge_segments(const std::vector<SegmentRef> &inputs, std::int64_t new_id, const MergeOptions &o = MergeOptions());

} // namespace vfts

#endif
