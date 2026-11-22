#!/usr/bin/env bash
# Download the raw benchmark datasets into $VFTS_DATA (default ~/vfts_data). Nothing from the
# datasets is stored in this repository. Check each license before you use the data:
#
#   msmarco   MS MARCO passage ranking: 8.8M passages, dev queries and judgements.
#             Microsoft terms: non-commercial research use. https://microsoft.github.io/msmarco/
#   enron     Enron email (CMU release of 2015-05-07), ~517k messages. Public record (FERC).
#             https://www.cs.cmu.edu/~enron/
#   pubmed    PubMed baseline files (NLM): 60 files, ~1.8M citations, about 1M with an abstract.
#             NLM terms: https://www.nlm.nih.gov/databases/download/terms_and_conditions.html
#   wiki      Arabic and Hebrew Wikipedia (CirrusSearch content dumps, plain text). Only the first
#             ~350k articles of each are downloaded (the stream is cut). CC BY-SA 4.0.
#
#   bench/data/fetch.sh [msmarco] [enron] [pubmed] [wiki]      (default: all)
#
# Downloads resume where they stopped (curl -C -) and are capped in time, so a stalled
# connection never hangs the script.
set -euo pipefail
DATA=${VFTS_DATA:-$HOME/vfts_data}
mkdir -p "$DATA/raw"
cd "$DATA/raw"
SETS=${*:-msmarco enron pubmed wiki}

get() {   # url [file]
    local url=$1 file=${2:-$(basename "$1")}
    [ -f "$file.done" ] && return 0
    echo "$(date +%T) fetch $file"
    for attempt in 1 2 3 4 5; do
        if curl -fsSL -C - --retry 3 --connect-timeout 30 --speed-limit 10000 --speed-time 120 \
                --max-time 7200 -o "$file" "$url"; then touch "$file.done"; return 0; fi
        echo "  retry $attempt for $file"; sleep 10
    done
    echo "failed: $url" >&2; return 1
}

for s in $SETS; do
  case $s in
    msmarco)
      B=https://msmarco.z22.web.core.windows.net/msmarcoranking
      get $B/collection.tar.gz
      get $B/queries.tar.gz
      get $B/qrels.dev.small.tsv ;;
    enron)
      get https://www.cs.cmu.edu/~enron/enron_mail_20150507.tar.gz ;;
    pubmed)
      for i in $(seq -f "%04g" 1 60); do get "https://ftp.ncbi.nlm.nih.gov/pubmed/baseline/pubmed26n$i.xml.gz"; done ;;
    wiki)
      # Plain-text dumps; the stream is cut after N articles (two JSON lines per article).
      W=https://dumps.wikimedia.org/other/cirrussearch/20251229
      for lang in he ar; do
        f=${lang}wiki-cirrus-head.json.gz
        [ -f "$f.done" ] && continue
        echo "$(date +%T) fetch $f (first 350k articles)"
        ( set +o pipefail
          curl -fsSL --connect-timeout 30 --speed-limit 10000 --speed-time 120 --max-time 7200 \
               "$W/${lang}wiki-20251229-cirrussearch-content.json.gz" | gzip -dc 2>/dev/null \
            | head -n 700000 | gzip -1 > "$f" )
        [ "$(gzip -dc "$f" | wc -l)" -ge 100000 ] && touch "$f.done" || { echo "short download: $f" >&2; exit 1; }
      done ;;
    *) echo "unknown dataset: $s" >&2; exit 2 ;;
  esac
done
echo "$(date +%T) all downloads done in $DATA/raw"
