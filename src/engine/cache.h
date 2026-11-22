// The per-node cache: copies of the index's segments, mmap'ed by the search functions.
//
//   <cache_dir>/<index>/seg_<segment_id>.vfs     one file per segment (segment.h), never changed
//   <cache_dir>/<index>/gen_<generation>.vfg     the active segments of a generation, their
//                                                tombstones and the tombstones' effect on document
//                                                frequencies (computed once, here), the access
//                                                mode and keys (mode 0600, like every file here)
//   <cache_dir>/<index>/ACTIVE                   the active generation number
//
// The cache is only a copy of the index tables: a missing or damaged file is never a data loss
// (CALL vfts.load_all('<index>') writes it again). Files are written to a temporary name,
// verified and renamed; ACTIVE is replaced last. The previous generation's files are kept, so a
// query that runs while a refresh activates a new generation finishes on the old one.
//
// In a process, the mapping of a segment file is kept and shared by later queries and later
// generations: a new generation maps only its new segments. What ACTIVE says is trusted for
// ACTIVE_CHECK_MS; a query whose view carries a newer generation reads it again at once.
#ifndef VFTS_ENGINE_CACHE_H
#define VFTS_ENGINE_CACHE_H

#include "search.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vfts {

constexpr const char *DEFAULT_CACHE_DIR = "/tmp/vfts";
constexpr int ACTIVE_CHECK_MS = 200;

// Letters, digits and underscore, 1 to 64 characters: an index name can never point outside
// the cache directory.
bool valid_index_name(const std::string &name);

struct GenSegment {
    std::int64_t segment_id = 0;
    std::uint64_t checksum = 0;             // the segment file's checksum (header)
    std::vector<std::uint32_t> dead;        // tombstoned docnos, ascending
};

struct Generation {
    std::int64_t gen = 0;
    std::string options;                    // the index's analyzer options, "lang=en;accent_fold=latin;..."
    std::vector<GenSegment> segs;
    // Who may search (docs/design.md "Access"): the access mode ("open", "rows" or
    // "masked:<column>") and the index's keys (the current one, during a key change also the
    // previous one). The views prove with a token made from a key that they are the input.
    std::string access;
    std::vector<std::string> keys;
};

class CacheWriter {
public:
    CacheWriter(const std::string &cache_dir, const std::string &index);
    const std::string &dir() const { return dir_; }

    // True when the cache holds the segment with this checksum (header check).
    bool has_segment(std::int64_t id, std::uint64_t checksum) const;
    // Verifies the bytes completely, then writes the file atomically.
    void write_segment(const std::string &bytes);
    // Writes the generation file (dead counts computed from the local segment files, or taken
    // from the previous generation when a segment's tombstones did not change), then ACTIVE.
    // Removes cache files that neither this nor the previous generation uses.
    void activate(const Generation &g);

private:
    std::string dir_;
};

// The active generation of an index on this node, ready to search.
struct LoadedIndex {
    std::int64_t gen = 0;
    std::string options;                    // as given to CacheWriter::activate
    std::string access;                     // as given to CacheWriter::activate ("" in files of vfts 0.1.0)
    std::vector<std::string> keys;
    std::vector<SegmentRef> refs;
    std::uint64_t bytes = 0, docs = 0, dead = 0;
    std::vector<std::shared_ptr<const void>> keep;      // mappings the refs point into
};

// Opens the active generation (cached in the process). min_gen: the generation the caller's view
// belongs to; a cache that is older is stale and an error names the node's problem. exact: open
// generation min_gen itself, also while a newer one is active (its files are kept until the next
// generation): for views whose rows belong to that generation (access mode "rows").
std::shared_ptr<const LoadedIndex> open_index(const std::string &cache_dir, const std::string &index, std::int64_t min_gen,
                                              bool exact = false);

struct CacheInfo {
    bool present = false;
    std::int64_t gen = 0;
    std::size_t segments = 0;
    std::uint64_t bytes = 0, docs = 0, dead = 0;
    std::string error;
};
CacheInfo cache_info(const std::string &cache_dir, const std::string &index);

// Removes an index's cache directory (only files that vfts writes).
void drop_cache(const std::string &cache_dir, const std::string &index);

} // namespace vfts

#endif
