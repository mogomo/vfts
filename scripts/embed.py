#!/usr/bin/env python3
"""Embeddings for semantic search next to vfts (README "Semantic search").

vfts includes no language model: this script computes the vectors outside Vertica, with any
sentence-transformers model, and writes them in a form that COPY loads into an ARRAY[FLOAT] column.

    input  (stdin):  one row per line, "<id><TAB><text>"  (vsql -At -F $'\\t' -c "SELECT id, text ...")
    output (stdout): one row per line, "<id>|[f1,f2,...]"  (COPY ... DELIMITER '|')

    pip install sentence-transformers
    vsql -At -F $'\\t' -c "SELECT doc_id, REGEXP_REPLACE(body, '\\s+', ' ') FROM ex.notes" \\
        | python3 scripts/embed.py > notes_vec.txt

The same model must embed the documents and the queries. Vectors are normalized to length 1, so
cosine similarity and the dot product give the same order.
"""
import argparse
import os
import sys
import warnings

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument("--model", default="sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2",
               help="a sentence-transformers model (default: multilingual, 384 dimensions)")
p.add_argument("--batch", type=int, default=64, help="texts per batch")
p.add_argument("--digits", type=int, default=6, help="decimals per value")
a = p.parse_args()

os.environ.setdefault("HF_HUB_DISABLE_PROGRESS_BARS", "1")
os.environ.setdefault("TRANSFORMERS_VERBOSITY", "error")
warnings.filterwarnings("ignore")
from sentence_transformers import SentenceTransformer  # noqa: E402  (after --help)

model = SentenceTransformer(a.model)
rows = []
for line in sys.stdin:
    line = line.rstrip("\n")
    if not line:
        continue
    key, _, text = line.partition("\t")
    rows.append((key, text))

for i in range(0, len(rows), a.batch):
    chunk = rows[i:i + a.batch]
    vecs = model.encode([t for _, t in chunk], batch_size=a.batch, normalize_embeddings=True, show_progress_bar=False)
    for (key, _), v in zip(chunk, vecs):
        sys.stdout.write(key + "|[" + ",".join(f"{x:.{a.digits}f}" for x in v) + "]\n")
    dim = len(vecs[0])
print(f"{len(rows)} rows, {dim if rows else 0} dimensions, model {a.model}", file=sys.stderr)
