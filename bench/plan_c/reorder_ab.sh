#!/usr/bin/env bash
# Document reordering, engine only (no SQL): the one-segment index of each dataset as loaded for the
# benchmark (documents in doc_id order) against the same segment reordered by recursive graph
# bisection (vfts_cli merge --force). Same queries and options as the benchmark; the hits of
# both must be identical (k = 10 and k = 100); then latency at 1 client (1 thread) and q/s at 32
# clients, the two indexes in turn, 3 rounds.
#
#   bench/plan_c/reorder_ab.sh [dataset ...]      default: enron pubmed msmarco wiki_he wiki_ar
#
# Reads the node cache of the Vertica indexes (/tmp/vfts/<dataset>), writes copies to
# ~/vfts_bp (removed at the end unless KEEP=1). Results: bench/out/plan_c/reorder_ab.jsonl.
set -uo pipefail
cd "$(dirname "$0")/../.."
DATA=${VFTS_DATA:-$HOME/vfts_data}
SRC=${VFTS_CACHE:-/tmp/vfts}
WORK=${VFTS_BP_DIR:-$HOME/vfts_bp}
OUT=bench/out/plan_c/reorder_ab.jsonl
CLI=build/tools/vfts_cli
SETS=${*:-enron pubmed msmarco wiki_he wiki_ar}
mkdir -p "$WORK" bench/out/plan_c
qfile() { case $1 in msmarco) echo "$DATA/msmarco/queries.dev.small.tsv" ;; *) echo "bench/queries/$1_x25.tsv" ;; esac; }
lfile() { case $1 in msmarco) echo "$DATA/msmarco/queries.dev.small.tsv" ;; *) echo "bench/queries/$1.tsv" ;; esac; }
qextra() { case $1 in msmarco) echo "--plain" ;; *) echo "" ;; esac; }
log() { echo "$(date +%T) $*"; }

for ds in $SETS; do
    log "== $ds"
    rm -rf "$WORK/${ds}_plain" "$WORK/${ds}_bp"
    mkdir -p "$WORK/${ds}_plain"
    g=$(cat "$SRC/$ds/ACTIVE")
    cp "$SRC/$ds/ACTIVE" "$SRC/$ds/gen_$g.vfg" "$WORK/${ds}_plain/"
    for f in $(ls "$SRC/$ds" | grep '^seg_'); do cp "$SRC/$ds/$f" "$WORK/${ds}_plain/"; done
    # only the active generation's segments matter; the plain copy keeps doc_id order
    cp -r "$WORK/${ds}_plain" "$WORK/${ds}_bp"
    m=$(timeout 7200 $CLI merge "$WORK" "${ds}_bp" 1 --force)
    echo "$m"
    p=$(timeout 7200 $CLI merge "$WORK" "${ds}_plain" 1 --force --no-reorder)
    echo "$p"
    # identical hits
    for k in 10 100; do
        for v in plain bp; do
            timeout 3600 $CLI search "$WORK" "${ds}_$v" "$(lfile $ds)" --k $k --threads 1 --repeat 1 --mode stem --max-expansions 100000 \
                $(qextra $ds) --run "$WORK/${ds}_$v.k$k.run" > /dev/null
        done
        if cmp -s "$WORK/${ds}_plain.k$k.run" "$WORK/${ds}_bp.k$k.run"; then same=true; else same=false; fi
        log "$ds k=$k identical hits: $same ($(wc -l < "$WORK/${ds}_bp.k$k.run") lines)"
        echo "{\"dataset\": \"$ds\", \"test\": \"same_hits_k$k\", \"same\": $same}" >> "$OUT"
    done
    for round in 1 2 3; do
        for v in plain bp; do
            l=$(timeout 3600 $CLI search "$WORK" "${ds}_$v" "$(lfile $ds)" --k 10 --threads 1 --repeat 3 --mode stem --max-expansions 100000 $(qextra $ds) | tail -1)
            c=$(timeout 3600 $CLI search "$WORK" "${ds}_$v" "$(qfile $ds)" --k 10 --clients 32 --repeat 2 --mode stem --max-expansions 100000 $(qextra $ds) | tail -1)
            log "$ds $v round $round: 1 client: $l"
            log "$ds $v round $round: 32 clients: $c"
            python3 -I -c 'import json,sys; print(json.dumps({"dataset": sys.argv[1], "order": sys.argv[2], "round": int(sys.argv[3]), "one_client": sys.argv[4], "c32": sys.argv[5], "merge": sys.argv[6]}))' \
                "$ds" "$v" "$round" "$l" "$c" "$( [ $v = bp ] && echo "$m" || echo "$p")" >> "$OUT"
        done
    done
    [ "${KEEP:-0}" = 1 ] || rm -rf "$WORK/${ds}_plain" "$WORK/${ds}_bp"
done
log FINISHED
