// vfts.hybrid(source, doc_id, rank, score USING PARAMETERS method, k, rrf_k, weights, distance) OVER()
//
// Fuses ranked lists by doc_id: typically the result of vfts.tsearch (BM25) and of vvector's
// vsearch (vectors), UNION ALLed with a source name in front (README "hybrid").
//   method 'rrf' (default)  Reciprocal Rank Fusion: score = sum over sources of w / (rrf_k + rank)
//   method 'weighted'       score = sum over sources of w * normalized score, where each source's
//                           scores are min-max normalized to [0, 1] (for sources named in
//                           `distance`, smaller scores are better and are inverted)
// weights: 'text=0.7,vector=0.3' (a source not named has weight 1). A document missing from a
// source gets nothing from it. Output: doc_id, score, rank (1 = best; ties by doc_id), and the
// ranks of the document in each source ('text:3 vector:1').
#include "udx_common.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

using namespace Vertica;
using namespace vfts_udx;

namespace {

struct Entry { std::string source; std::int64_t doc_id, rank; double score; bool has_score; };

std::map<std::string, double> parse_weights(const std::string &s)
{
    std::map<std::string, double> w;
    std::istringstream in(s);
    std::string item;
    while (std::getline(in, item, ',')) {
        const std::size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string k = item.substr(0, eq);
        k.erase(0, k.find_first_not_of(' '));
        k.erase(k.find_last_not_of(' ') + 1);
        w[k] = std::stod(item.substr(eq + 1));
    }
    return w;
}

class Hybrid : public TransformFunction {
public:
    void processPartition(ServerInterface &srv, PartitionReader &in, PartitionWriter &out) override
    {
        try {
            Settings s(srv);
            const std::string method = s.text("method", "rrf");
            if (method != "rrf" && method != "weighted") fail("method must be 'rrf' or 'weighted'");
            const double rrf_k = s.real("rrf_k", 60.0);
            const std::int64_t k = s.integer("k", 10);
            if (k < 1 || k > 1000000) fail("k must be 1 to 1000000");
            const auto weights = parse_weights(s.text("weights", ""));
            std::set<std::string> distance;
            {
                std::istringstream d(s.text("distance", ""));
                std::string x;
                while (std::getline(d, x, ',')) if (!x.empty()) distance.insert(x);
            }
            std::vector<Entry> rows;
            do {
                if (in.isNull(0) || in.isNull(1)) continue;
                Entry e;
                e.source = in.getStringRef(0).str();
                e.doc_id = in.getIntRef(1);
                e.rank = in.isNull(2) ? 0 : in.getIntRef(2);
                e.has_score = !in.isNull(3);
                e.score = e.has_score ? in.getFloatRef(3) : 0.0;
                rows.push_back(e);
            } while (in.next());
            // per source: min and max score, and ranks when they are missing (by score)
            std::map<std::string, std::pair<double, double>> range;
            for (const Entry &e : rows) {
                if (!e.has_score) continue;
                auto it = range.find(e.source);
                if (it == range.end()) range[e.source] = {e.score, e.score};
                else { it->second.first = std::min(it->second.first, e.score); it->second.second = std::max(it->second.second, e.score); }
            }
            std::map<std::int64_t, double> fused;
            std::map<std::int64_t, std::string> where;
            for (const Entry &e : rows) {
                auto wi = weights.find(e.source);
                const double w = wi == weights.end() ? 1.0 : wi->second;
                double v;
                if (method == "rrf") {
                    if (e.rank < 1) fail("rrf needs the rank column (1 = best) of source " + e.source);
                    v = w / (rrf_k + static_cast<double>(e.rank));
                } else {
                    if (!e.has_score) fail("weighted needs the score column of source " + e.source);
                    const auto r = range[e.source];
                    double norm = r.second > r.first ? (e.score - r.first) / (r.second - r.first) : 1.0;
                    if (distance.count(e.source)) norm = r.second > r.first ? (r.second - e.score) / (r.second - r.first) : 1.0;
                    v = w * norm;
                }
                fused[e.doc_id] += v;
                std::string &tag = where[e.doc_id];
                tag += (tag.empty() ? "" : " ") + e.source + ":" + std::to_string(e.rank);
            }
            std::vector<std::pair<double, std::int64_t>> v;
            for (auto &kv : fused) v.push_back({kv.second, kv.first});
            std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
            if (static_cast<std::int64_t>(v.size()) > k) v.resize(static_cast<std::size_t>(k));
            vint rank = 0;
            for (auto &x : v) {
                out.setInt(0, x.second);
                out.setFloat(1, x.first);
                out.setInt(2, ++rank);
                const std::string &t = where[x.second];
                out.getStringRef(3).copy(t.data(), std::min<std::size_t>(t.size(), 1000));
                out.next();
            }
        } catch (const std::exception &e) {
            vt_report_error(0, "hybrid: %s", e.what());
        }
    }
};

class HybridFactory : public TransformFunctionFactory {
public:
    void getPrototype(ServerInterface &, ColumnTypes &a, ColumnTypes &r) override
    {
        a.addVarchar(); a.addInt(); a.addInt(); a.addFloat();
        r.addInt(); r.addFloat(); r.addInt(); r.addVarchar();
    }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override
    {
        out.addInt("doc_id");
        out.addFloat("score");
        out.addInt("rank");
        out.addVarchar(1000, "sources");
    }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        p.addVarchar(16, "method");
        p.addInt("k");
        p.addFloat("rrf_k");
        p.addVarchar(1000, "weights");
        p.addVarchar(1000, "distance");
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override { return vt_createFuncObject<Hybrid>(srv.allocator); }
};

} // namespace

RegisterFactory(HybridFactory);
