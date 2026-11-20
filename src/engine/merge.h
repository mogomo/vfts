// Merging segments: the live documents of several segments (tombstoned ones dropped) into one new
// segment, without analyzing any text again. Postings and positions are merged term by term.
#ifndef VTEXT_ENGINE_MERGE_H
#define VTEXT_ENGINE_MERGE_H

#include "search.h"

#include <string>
#include <vector>

namespace vtext {

// The new segment's bytes. Every doc_id must be live in at most one input segment.
std::string merge_segments(const std::vector<SegmentRef> &inputs, std::int64_t new_id);

} // namespace vtext

#endif
