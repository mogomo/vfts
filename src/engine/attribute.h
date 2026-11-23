// Attributes: a column of another table (custodian, status, sent date) copied into the index, so
// that a search filters and counts by it while it matches, as search engines do with doc values.
//
//   <cache_dir>/<index>/attr_<name>.vfa      one file per attribute, written by
//                                            CALL vfts.refresh_attributes (tattr), replaced whole
//
// The table stays the place where the value is kept and changed; the file is a copy made by a
// statement (docs/design.md "Attributes"). A document without a row (or with NULL) has no value:
// a filter leaves it out (SQL's WHERE), a count does not count it (an inner join's GROUP BY).
//
// File (host byte order):
//   AttrHeader (64 bytes)
//   int64  doc_id[n_rows]       ascending, unique
//   uint32 code[n_rows]         the value of each row, as its position in the dictionary
//   (padding to 8 bytes)
//   dictionary, ascending and unique:
//     int, bool, date: int64 value[n_codes]  (bool 0/1; date: days since 2000-01-01, Vertica's DateADT)
//     text:            uint64 offset[n_codes + 1], then the bytes (byte order, Vertica's binary collation)
// Codes follow value order, so a range of values is a range of codes.
#ifndef VFTS_ENGINE_ATTRIBUTE_H
#define VFTS_ENGINE_ATTRIBUTE_H

#include "search.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vfts {

enum class AttrType : std::uint32_t { Int = 0, Bool = 1, Date = 2, Text = 3 };
constexpr std::uint32_t ATTR_NONE = 0xFFFFFFFFu;

const char *attr_type_name(AttrType t);
bool parse_attr_type(const std::string &s, AttrType &t);
std::string attr_file_name(const std::string &name);

// Days since 2000-01-01 of 'YYYY-MM-DD' (false when it is not a valid date), and back.
bool parse_date(std::string_view s, std::int64_t &days);
std::string format_date(std::int64_t days);

// The bytes of an attribute file. Rows in any order; a doc_id twice is an error.
std::string encode_attribute(AttrType type, std::int64_t version, std::vector<std::pair<std::int64_t, std::int64_t>> ints);
std::string encode_attribute_text(std::int64_t version, std::vector<std::pair<std::int64_t, std::string>> texts);

class Attribute {
public:
    // Checks the header, the sizes and the checksum. The bytes must outlive the object
    // (keep: an owner of them, e.g. a mapping).
    Attribute(const char *p, std::size_t n, std::shared_ptr<const void> keep = nullptr);

    AttrType type() const { return type_; }
    std::int64_t version() const { return version_; }
    std::uint64_t rows() const { return n_rows_; }
    std::uint32_t n_codes() const { return n_codes_; }
    // Unique in the process: this object (the filter cache's key, never reused).
    std::uint64_t serial() const { return serial_; }

    std::uint32_t code_of(std::int64_t doc_id) const;
    std::int64_t int_value(std::uint32_t code) const { return ints_[code]; }
    std::string_view text_value(std::uint32_t code) const
    {
        return std::string_view(pool_ + offs_[code], static_cast<std::size_t>(offs_[code + 1] - offs_[code]));
    }
    // The value as Vertica prints it: 42, t / f, 2001-06-01, text.
    std::string value_string(std::uint32_t code) const;

    // Code of every docno of a segment. Kept for segments with an id (not for a live tail).
    std::shared_ptr<const std::vector<std::uint32_t>> codes_for(const Segment &seg) const;

    // Codes of values v with lo <= v < hi in value order: [first, last) for the given bounds.
    std::uint32_t lower_int(std::int64_t v) const;
    std::uint32_t lower_text(std::string_view v) const;

private:
    AttrType type_ = AttrType::Int;
    std::int64_t version_ = 0;
    std::uint64_t n_rows_ = 0;
    std::uint32_t n_codes_ = 0;
    const std::int64_t *ids_ = nullptr;
    const std::uint32_t *codes_ = nullptr;
    const std::int64_t *ints_ = nullptr;
    const std::uint64_t *offs_ = nullptr;
    const char *pool_ = nullptr;
    std::shared_ptr<const void> keep_;
    std::uint64_t serial_ = 0;
    // doc_id -> code by direct index when the ids are dense enough (else binary search)
    std::int64_t dense_lo_ = 0;
    std::vector<std::uint32_t> dense_;
    mutable std::mutex mu_;
    mutable std::map<std::pair<std::int64_t, std::uint64_t>, std::shared_ptr<const std::vector<std::uint32_t>>> per_seg_;
};

// The attribute's file in this node's cache (kept in the process; a replaced file is read again
// within ACTIVE_CHECK_MS). Throws with the reason when it is missing.
std::shared_ptr<const Attribute> open_attribute(const std::string &cache_dir, const std::string &index, const std::string &name);
// Forgets the files of an index kept in this process (its mappings are released when no search
// uses them any more).
void forget_attributes(const std::string &cache_dir, const std::string &index);
// The names of the attribute files of an index in this node's cache.
std::vector<std::string> attribute_names(const std::string &cache_dir, const std::string &index);

// A filter over attributes, SQL's WHERE syntax for one table:
//   custodian IN (3, 17) AND NOT (status = 'closed' OR sent < '2001-06-01')
//   =  !=  <>  <  <=  >  >=   IN (..)   NOT IN (..)   BETWEEN a AND b   IS NULL   IS NOT NULL
//   AND  OR  NOT  ( )         literals: integers, 'text' ('' for a quote), TRUE, FALSE, DATE 'YYYY-MM-DD'
// Comparisons with a missing value are unknown, as in SQL: a document is kept only when the whole
// filter is true.
class Filter {
public:
    using Opener = std::function<std::shared_ptr<const Attribute>(const std::string &)>;
    // Throws with a message that names the place of the error.
    static std::shared_ptr<const Filter> parse(const std::string &text, const Opener &open);

    // The documents of a segment the filter leaves out (null when it keeps them all). With cache,
    // the result is kept for this segment and these attribute files (filter cache, LRU, 512 MB).
    std::shared_ptr<const DocSet> excluded(const Segment &seg, bool cache = true) const;
    // The filter asked per candidate document (SearchOptions::doc_filter), for searches without
    // the filter cache: the cost follows the matches, not the size of the collection. The filter
    // must outlive the view.
    std::unique_ptr<DocFilter> view(const std::vector<const Segment *> &segs) const;
    const std::string &text() const { return text_; }
    // The text and the attribute files it reads (name, version, the object's serial): equal keys
    // give equal results on the same segments.
    const std::string &key() const { return key_; }

    struct Node;
    ~Filter();

private:
    std::string text_, key_;
    std::vector<std::shared_ptr<const Attribute>> attrs_;
    std::vector<std::string> attr_names_;
    std::unique_ptr<Node> root_;
    std::shared_ptr<const DocSet> compute(const Segment &seg) const;
};

// The filter sets of every segment of a searcher, for SearchOptions::exclude.
std::vector<std::shared_ptr<const DocSet>> filter_segments(const Filter &f, const std::vector<const Segment *> &segs, bool cache = true);

} // namespace vfts

#endif
