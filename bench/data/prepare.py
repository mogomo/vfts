#!/usr/bin/env python3
"""Turn the raw downloads (bench/data/fetch.sh) into one load file per dataset.

    python3 -I bench/data/prepare.py [msmarco] [enron] [pubmed] [wiki_he] [wiki_ar]   (default: all)

Input:  $VFTS_DATA/raw (default ~/vfts_data/raw)
Output: $VFTS_DATA/<dataset>/docs.dat   one record per document:
            doc_id \\x01 lang \\x01 body \\x02
        (control characters \\x00 \\x01 \\x02 are removed from the text, nothing else is changed),
        $VFTS_DATA/<dataset>/stats.txt   documents and bytes
        $VFTS_DATA/msmarco/queries.dev.small.tsv, qrels.dev.small.tsv   (MS MARCO only)

The same file is loaded into Vertica (COPY) and into Elasticsearch / OpenSearch (bench/load_engines.py),
so every engine indexes exactly the same text. Python standard library only.
"""
import email
import email.policy
import gzip
import json
import os
import sys
import tarfile
import time
import xml.etree.ElementTree as ET

DATA = os.environ.get("VFTS_DATA", os.path.expanduser("~/vfts_data"))
RAW = os.path.join(DATA, "raw")
LIMITS = {"pubmed": 1_000_000, "wiki_he": 300_000, "wiki_ar": 300_000}
MAX_BODY = 1_000_000          # characters per document (Enron has a few huge attachments)
CLEAN = str.maketrans({"\x00": " ", "\x01": " ", "\x02": " "})


class Writer:
    def __init__(self, name):
        self.dir = os.path.join(DATA, name)
        os.makedirs(self.dir, exist_ok=True)
        self.tmp = os.path.join(self.dir, "docs.dat.part")
        self.f = open(self.tmp, "w", encoding="utf-8", newline="")
        self.n = 0
        self.bytes = 0
        self.t0 = time.time()

    def add(self, doc_id, lang, body):
        body = body.translate(CLEAN)[:MAX_BODY]
        rec = f"{doc_id}\x01{lang}\x01{body}\x02"
        self.f.write(rec)
        self.n += 1
        self.bytes += len(body.encode("utf-8"))
        if self.n % 200000 == 0:
            print(f"  {self.n} documents", flush=True)

    def close(self):
        self.f.close()
        os.replace(self.tmp, os.path.join(self.dir, "docs.dat"))
        with open(os.path.join(self.dir, "stats.txt"), "w") as s:
            s.write(f"documents {self.n}\ntext_bytes {self.bytes}\n")
        print(f"  {self.n} documents, {self.bytes / 1e6:.0f} MB of text, {time.time() - self.t0:.0f} s", flush=True)


def msmarco():
    w = Writer("msmarco")
    with tarfile.open(os.path.join(RAW, "collection.tar.gz"), "r|gz") as t:
        for m in t:
            if m.name.endswith("collection.tsv"):
                for line in t.extractfile(m):
                    pid, _, text = line.decode("utf-8").rstrip("\n").partition("\t")
                    w.add(int(pid), "en", text)
    w.close()
    qrels = os.path.join(RAW, "qrels.dev.small.tsv")
    want = set()
    with open(qrels) as f, open(os.path.join(w.dir, "qrels.dev.small.tsv"), "w") as out:
        for line in f:
            out.write(line)
            want.add(line.split("\t")[0])
    n = 0
    with tarfile.open(os.path.join(RAW, "queries.tar.gz"), "r|gz") as t, \
            open(os.path.join(w.dir, "queries.dev.small.tsv"), "w", encoding="utf-8") as out:
        for m in t:
            if m.name.endswith("queries.dev.tsv"):
                for line in t.extractfile(m):
                    qid, _, text = line.decode("utf-8").rstrip("\n").partition("\t")
                    if qid in want:
                        out.write(f"{qid}\t{text}\n")
                        n += 1
    print(f"  {n} dev queries (small), {len(want)} judged", flush=True)


def _text_of(msg):
    if msg.is_multipart():
        parts = []
        for p in msg.walk():
            if p.get_content_type() == "text/plain" and not p.is_multipart():
                parts.append(_decode(p))
        return "\n".join(parts)
    return _decode(msg)


def _decode(part):
    raw = part.get_payload(decode=True) or b""
    for enc in (part.get_content_charset() or "utf-8", "latin-1"):
        try:
            return raw.decode(enc)
        except (LookupError, UnicodeDecodeError):
            continue
    return raw.decode("latin-1")


def enron():
    w = Writer("enron")
    doc_id = 0
    with tarfile.open(os.path.join(RAW, "enron_mail_20150507.tar.gz"), "r|gz") as t:
        for m in t:
            if not m.isfile():
                continue
            data = t.extractfile(m).read()
            msg = email.message_from_bytes(data, policy=email.policy.compat32)
            subject = str(msg.get("Subject", "") or "")
            doc_id += 1
            w.add(doc_id, "en", subject + "\n" + _text_of(msg))
    w.close()


def pubmed():
    w = Writer("pubmed")
    files = sorted(f for f in os.listdir(RAW) if f.startswith("pubmed") and f.endswith(".xml.gz"))
    seen = set()
    for name in files:
        with gzip.open(os.path.join(RAW, name)) as f:
            for _, el in ET.iterparse(f):
                if el.tag != "PubmedArticle":
                    continue
                pmid = el.findtext("MedlineCitation/PMID")
                art = el.find("MedlineCitation/Article")
                abstract = ""
                title = ""
                if art is not None:
                    t = art.find("ArticleTitle")
                    title = "".join(t.itertext()) if t is not None else ""
                    abstract = " ".join("".join(a.itertext()) for a in art.findall("Abstract/AbstractText"))
                el.clear()
                if not pmid or not abstract.strip() or pmid in seen:
                    continue
                seen.add(pmid)
                w.add(int(pmid), "en", title + "\n" + abstract)
                if w.n >= LIMITS["pubmed"]:
                    w.close()
                    return
    w.close()


def wiki(lang):
    name = f"wiki_{lang}"
    w = Writer(name)
    with gzip.open(os.path.join(RAW, f"{lang}wiki-cirrus-head.json.gz"), "rt", encoding="utf-8") as f:
        page_id = None
        for line in f:
            try:
                j = json.loads(line)
            except ValueError:
                break           # the stream was cut inside the last line
            if "index" in j:
                page_id = j["index"].get("_id")
                continue
            if j.get("namespace", 0) != 0 or not page_id:
                continue
            body = (j.get("title") or "") + "\n" + (j.get("text") or "")
            w.add(int(page_id), lang, body)
            if w.n >= LIMITS[name]:
                break
    w.close()


SETS = {"msmarco": msmarco, "enron": enron, "pubmed": pubmed,
        "wiki_he": lambda: wiki("he"), "wiki_ar": lambda: wiki("ar")}

if __name__ == "__main__":
    for s in sys.argv[1:] or list(SETS):
        if s not in SETS:
            sys.exit(f"unknown dataset {s}; one of {', '.join(SETS)}")
        print(f"{s}:", flush=True)
        SETS[s]()
