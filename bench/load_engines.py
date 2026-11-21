#!/usr/bin/env python3
"""Load a dataset into Elasticsearch or OpenSearch with analyzers equivalent to vtext's.

    python3 -I bench/load_engines.py <es|os> <dataset> [--shards N] [--workers W] [--no-merge]

Reads $VTEXT_DATA/<dataset>/docs.dat (bench/data/prepare.py), creates the index <dataset>
(dropped first if it exists), bulk-loads with W parallel workers, refreshes, force-merges to one
segment per shard (unless --no-merge), and prints one JSON line with the timings and sizes, which
bench/run_all.sh appends to bench/results/<engine>_load.jsonl.

Equivalence with vtext (docs/design.md "Analysis"):
  tokenizer   icu_tokenizer                         (ICU word boundaries, UAX #29)
  normalize   icu_normalizer (nfkc_cf)              (NFKC + case folding)
              apostrophes ' ʼ ＇ -> '               (char filter, vtext unifies them too)
              Latin accents removed                 (icu_transform restricted to Latin letters)
              Arabic / Persian normalization        (the Lucene filters vtext follows)
              Hebrew points removed, ״ -> " ׳ -> '   (pattern_replace / char filter)
  stem        English: snowball English on field body (vtext: mode='stem', expansion by stem)
              body.exact: no stemming                (vtext: mode='exact', and prefix/wildcard/fuzzy)
  similarity  BM25, k1 = 1.2, b = 0.75 (the default of both engines)
Python standard library only.
"""
import http.client
import json
import multiprocessing as mp
import os
import sys
import time

DATA = os.environ.get("VTEXT_DATA", os.path.expanduser("~/vtext_data"))
PORTS = {"es": int(os.environ.get("ES_PORT", 9200)), "os": int(os.environ.get("OS_PORT", 9201))}
LANG = {"msmarco": "en", "enron": "en", "pubmed": "en", "wiki_he": "he", "wiki_ar": "ar"}


def request(port, method, path, body=None, timeout=3600):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
    c.request(method, path, body=data, headers={"Content-Type": "application/x-ndjson" if path.endswith("_bulk") else "application/json"})
    r = c.getresponse()
    out = r.read()
    c.close()
    if r.status >= 300 and not (method == "DELETE" and r.status == 404):
        raise RuntimeError(f"{method} {path}: HTTP {r.status}: {out[:500]!r}")
    return json.loads(out) if out else {}


def settings(lang, shards):
    char_filters = {"vt_quotes": {"type": "mapping", "mappings": ["\\u2019=>'", "\\u02BC=>'", "\\uFF07=>'", "\\u05F4=>\"", "\\u05F3=>'"]}}
    filters = {
        "vt_latin_fold": {"type": "icu_transform", "id": "[\\u0041-\\u024F\\u1E00-\\u1EFF\\u0300-\\u036F] NFD; [:Nonspacing Mark:] Remove; NFC"},
        "vt_hebrew_points": {"type": "pattern_replace", "pattern": "[\\u0591-\\u05C7]", "replacement": ""},
        "vt_arabic_extra": {"type": "pattern_replace", "pattern": "[\\u0654]", "replacement": ""},
        "vt_snowball_en": {"type": "snowball", "language": "English"},
    }
    base = ["icu_normalizer", "vt_latin_fold"]
    if lang == "ar":
        base += ["arabic_normalization", "persian_normalization", "vt_arabic_extra"]
    if lang == "he":
        base += ["vt_hebrew_points"]
    stem = base + (["vt_snowball_en"] if lang == "en" else ["arabic_stem"] if lang == "ar" else [])
    if lang == "ar":
        filters["arabic_stem"] = {"type": "stemmer", "language": "arabic"}
    analyzers = {
        "vt_exact": {"type": "custom", "tokenizer": "icu_tokenizer", "char_filter": ["vt_quotes"], "filter": base},
        "vt_stem": {"type": "custom", "tokenizer": "icu_tokenizer", "char_filter": ["vt_quotes"], "filter": stem},
    }
    return {
        "settings": {
            "number_of_shards": shards, "number_of_replicas": 0, "refresh_interval": "-1",
            "analysis": {"char_filter": char_filters, "filter": filters, "analyzer": analyzers},
            "similarity": {"default": {"type": "BM25", "k1": 1.2, "b": 0.75}},
        },
        "mappings": {
            "dynamic": "strict",
            "properties": {
                "doc_id": {"type": "long"},
                "body": {"type": "text", "analyzer": "vt_stem", "fields": {"exact": {"type": "text", "analyzer": "vt_exact"}}},
            },
        },
    }


def records(path, start, end):
    """Records whose first byte is in [start, end)."""
    with open(path, "rb") as f:
        f.seek(start)
        if start > 0:
            # move to the start of the next record
            while True:
                b = f.read(1)
                if not b or b == b"\x02":
                    break
        buf = b""
        pos = f.tell()
        while pos < end:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            buf += chunk
            while True:
                i = buf.find(b"\x02")
                if i < 0:
                    break
                rec, buf = buf[:i], buf[i + 1:]
                if pos >= end:
                    return
                pos += i + 1
                doc_id, _, rest = rec.partition(b"\x01")
                _, _, body = rest.partition(b"\x01")
                yield int(doc_id), body.decode("utf-8")


def worker(args):
    port, index, path, start, end = args
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=3600)
    lines, size, n = [], 0, 0

    def flush():
        nonlocal lines, size
        if not lines:
            return
        c.request("POST", f"/{index}/_bulk", body=b"".join(lines), headers={"Content-Type": "application/x-ndjson"})
        r = c.getresponse()
        out = json.loads(r.read())
        if r.status >= 300 or out.get("errors"):
            first = next((i for i in out.get("items", []) if list(i.values())[0].get("error")), out)
            raise RuntimeError(f"bulk failed: {json.dumps(first)[:500]}")
        lines, size = [], 0

    for doc_id, body in records(path, start, end):
        a = json.dumps({"index": {"_id": str(doc_id)}}).encode() + b"\n"
        d = json.dumps({"doc_id": doc_id, "body": body}, ensure_ascii=False).encode() + b"\n"
        lines += [a, d]
        size += len(a) + len(d)
        n += 1
        if size >= 8 << 20:
            flush()
    flush()
    c.close()
    return n


def main():
    engine, ds = sys.argv[1], sys.argv[2]
    shards = int(sys.argv[sys.argv.index("--shards") + 1]) if "--shards" in sys.argv else 1
    workers = int(sys.argv[sys.argv.index("--workers") + 1]) if "--workers" in sys.argv else 16
    merge = "--no-merge" not in sys.argv
    port = PORTS[engine]
    path = os.path.join(DATA, ds, "docs.dat")
    request(port, "DELETE", f"/{ds}")
    request(port, "PUT", f"/{ds}", settings(LANG[ds], shards))
    size = os.path.getsize(path)
    parts = workers * 4
    ranges = [(port, ds, path, size * i // parts, size * (i + 1) // parts) for i in range(parts)]
    t0 = time.time()
    with mp.Pool(workers) as pool:
        docs = sum(pool.map(worker, ranges, chunksize=1))
    t_bulk = time.time() - t0
    t1 = time.time()
    request(port, "POST", f"/{ds}/_refresh")
    t_refresh = time.time() - t1
    t2 = time.time()
    if merge:
        request(port, "POST", f"/{ds}/_forcemerge?max_num_segments=1", timeout=7200)
    t_merge = time.time() - t2
    request(port, "PUT", f"/{ds}/_settings", {"index": {"refresh_interval": "1s"}})
    stats = request(port, "GET", f"/{ds}/_stats/store,docs,segments")["indices"][ds]["primaries"]
    print(json.dumps({"engine": engine, "dataset": ds, "shards": shards, "workers": workers, "docs": docs,
                      "count": stats["docs"]["count"], "bulk_s": round(t_bulk, 1), "refresh_s": round(t_refresh, 1),
                      "merge_s": round(t_merge, 1), "index_s": round(t_bulk + t_refresh + t_merge, 1),
                      "bytes": stats["store"]["size_in_bytes"], "segments": stats["segments"]["count"]}))


if __name__ == "__main__":
    main()
