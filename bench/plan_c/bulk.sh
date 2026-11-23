#!/usr/bin/env bash
# Bulk searches: whole query sets at once (a report, a nightly run), vfts against Elasticsearch
# and OpenSearch. One engine runs at a time. Every result is one JSON line in
# bench/out/bulk/results.jsonl (with the CPU used, cpuwrap.py).
#
#   bench/plan_c/bulk.sh [step ...]     steps: b1 (default), b2, b3
#
#   b1   four sets, each timed until every result is stored (vfts: a file or a table; engines: a file):
#          msmarco_top10   the 6,980 MS MARCO dev questions, top 10 each (plain text)
#          enron_top10     the 500 Enron search terms (bench/queries/enron_terms.tsv), top 10 each
#          enron_count     the same 500 terms, the number of matching emails of each
#          msmarco_count   the 6,980 questions, the number of matching passages of each
#        vfts: one statement with all the queries (parameter queries), or P statements at once with
#        1/P each; also into a table (INSERT + COMMIT). Engines: _msearch, several batch sizes and
#        numbers of requests at once; the best of each is the engine's result. Counts run with the
#        engines' caches off (request cache and node query cache), as a report runs each term once.
#   b1cold  each set once, right after a restart of the database or the engine (no warm-up): vfts
#        makes the bit sets of its frequent words in that first run (VFTS_DB: the database, default VDB)
#   b2   many users' batches (bulk_shared.py): each user's own statement, or one service user for all,
#        results in one table with a row access policy (requester = CURRENT_USER())
#   b3   a cluster: the same batch on 1, 2, ... nodes, one statement per node (every node keeps a full
#        copy of the index; a statement runs on the node it connects to). The Enron terms BULK_X times
#        (default 10: 5,000 queries); BULK_INTO: the results into that table instead of the client. BULK_HOSTS: the nodes' addresses, comma separated (never in the repository).
#        Run it on a node of the cluster, with the index enron loaded (bench/load_vertica.sh enron).
set -uo pipefail
cd "$(dirname "$0")/../.."
O=bench/out/bulk
mkdir -p "$O"
RES=${BULK_RES:-$O/results.jsonl}
DATA=${VFTS_DATA:-$HOME/vfts_data}
STEPS=${*:-b1}
REPEAT=${BULK_REPEAT:-3}
log() { echo "$(date +%T) $*"; }
W() { local t=$1; shift; timeout 3600 python3 -I bench/plan_c/cpuwrap.py "$RES" "$t" -- "$@"; }
governor() {
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo "$1" | sudo -n tee "$g" > /dev/null 2>&1 || break; done
    log "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2> /dev/null)"
}
TERMS=bench/queries/enron_terms.tsv
MSQ=$DATA/msmarco/queries.dev.small.tsv
VP="--param max_expansions=100000"      # as in the other tests: prefixes expand like the engines' prefix queries

set_args() {   # set -> "index queries flags"
    case $1 in
        msmarco_top10) echo "msmarco $MSQ --plain" ;;
        enron_top10)   echo "enron $TERMS" ;;
        enron_count)   echo "enron $TERMS --count" ;;
        msmarco_count) echo "msmarco $MSQ --plain --count" ;;
    esac
}

qcache() {   # port index true|false: the node query cache of an index (a static setting: close, set, open)
    curl -s -XPOST "localhost:$1/$2/_close" > /dev/null
    curl -s -XPUT "localhost:$1/$2/_settings" -H 'Content-Type: application/json' -d "{\"index.queries.cache.enabled\": $3}" > /dev/null
    curl -s -XPOST "localhost:$1/$2/_open" > /dev/null
    curl -s "localhost:$1/_cluster/health/$2?wait_for_status=yellow&timeout=120s" > /dev/null
}

step_b1() {
    local old; old=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2> /dev/null || echo none)
    governor performance
    local sets=${BULK_SETS:-msmarco_top10 enron_top10 enron_count msmarco_count}
    bench/engines/engines.sh stop all > /dev/null 2>&1
    for s in $sets; do
        # shellcheck disable=SC2046
        set -- $(set_args "$s")
        local idx=$1 qf=$2; shift 2
        for p in ${BULK_SPLITS:-1 2 4 8}; do
            log "b1 vfts $s split $p"
            W "b1_${s}_vfts_file_p$p" python3 -I bench/plan_c/bulk.py vfts "$idx" "$qf" "$@" $VP --split "$p" --repeat "$REPEAT" --out "$O/${s}_vfts.tsv"
        done
        for p in 1 4; do
            log "b1 vfts $s into a table, split $p"
            W "b1_${s}_vfts_table_p$p" python3 -I bench/plan_c/bulk.py vfts "$idx" "$qf" "$@" $VP --split "$p" --repeat "$REPEAT" --into bench.bulk_results
        done
    done
    vsql -X -q -c "DROP TABLE IF EXISTS bench.bulk_results CASCADE;"
    for e in ${BULK_ENGINES-es os}; do
        local port=9200; [ "$e" = os ] && port=9201
        bench/engines/engines.sh stop all > /dev/null 2>&1
        timeout 600 bench/engines/engines.sh start "$e" > /dev/null || { log "b1: $e did not start"; continue; }
        for s in $sets; do
            # shellcheck disable=SC2046
            set -- $(set_args "$s")
            local idx=$1 qf=$2; shift 2
            local nc=""
            case $s in *_count) nc="--no-cache"; qcache "$port" "$idx" false ;; esac
            local n; n=$(wc -l < "$qf")
            for cfg in "100 1" "100 8" "100 22" "1000 4" "$n 1" "$n 1 22"; do
                # shellcheck disable=SC2086
                set -- $cfg
                local b=$1 c=$2 m=${3:-}
                local tag="b${b}_c${c}${m:+_mcs$m}"
                log "b1 $e $s $tag"
                set -- $(set_args "$s"); shift 2
                # shellcheck disable=SC2086
                W "b1_${s}_${e}_$tag" python3 -I bench/plan_c/bulk.py "$e" "$idx" "$qf" "$@" --batch "$b" --clients "$c" ${m:+--mcs $m} $nc \
                    --repeat "$REPEAT" --out "$O/${s}_${e}.tsv"
            done
            case $s in *_count) qcache "$port" "$idx" true ;; esac
        done
        bench/engines/engines.sh stop "$e" > /dev/null
    done
    governor "$old"
}

step_b1cold() {
    local old; old=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2> /dev/null || echo none)
    governor performance
    local sets=${BULK_SETS:-msmarco_top10 enron_top10 enron_count msmarco_count}
    bench/engines/engines.sh stop all > /dev/null 2>&1
    for s in $sets; do
        # shellcheck disable=SC2046
        set -- $(set_args "$s")
        local idx=$1 qf=$2; shift 2
        log "b1cold vfts $s: restart the database"
        timeout 600 /opt/vertica/bin/admintools -t stop_db -d "${VFTS_DB:-VDB}" -F > /dev/null 2>&1
        timeout 900 /opt/vertica/bin/admintools -t start_db -d "${VFTS_DB:-VDB}" > /dev/null 2>&1
        W "b1cold_${s}_vfts_file_p4" python3 -I bench/plan_c/bulk.py vfts "$idx" "$qf" "$@" $VP --split 4 --warmup 0 --repeat 1
    done
    for e in ${BULK_ENGINES-es os}; do
        local port=9200; [ "$e" = os ] && port=9201
        for s in $sets; do
            # shellcheck disable=SC2046
            set -- $(set_args "$s")
            local idx=$1 qf=$2; shift 2
            bench/engines/engines.sh stop all > /dev/null 2>&1
            timeout 600 bench/engines/engines.sh start "$e" > /dev/null || { log "b1cold: $e did not start"; continue; }
            local nc=""
            case $s in *_count) nc="--no-cache"; qcache "$port" "$idx" false ;; esac
            log "b1cold $e $s"
            W "b1cold_${s}_${e}_b100_c8" python3 -I bench/plan_c/bulk.py "$e" "$idx" "$qf" "$@" --batch 100 --clients 8 $nc --warmup 0 --repeat 1
            case $s in *_count) qcache "$port" "$idx" true ;; esac
        done
        bench/engines/engines.sh stop "$e" > /dev/null
    done
    governor "$old"
}

step_b2() {
    for s in ${BULK_SHARED_SETS:-enron_top10 msmarco_top10}; do
        # shellcheck disable=SC2046
        set -- $(set_args "$s")
        local idx=$1 qf=$2; shift 2
        log "b2 $s"
        W "b2_${s}" python3 -I bench/plan_c/bulk_shared.py "$idx" "$qf" "$@" --users 10 --split 4 --repeat "$REPEAT"
    done
}

step_b3() {
    local hosts=${BULK_HOSTS:?BULK_HOSTS: the nodes, comma separated}
    local x=${BULK_X:-10}                     # the 500 terms x times (qids r * 1000 + qid)
    local q=$O/enron_terms_x$x.tsv
    for r in $(seq 0 $((x - 1))); do awk -F'\t' -v r="$r" '{ printf "%d\t%s\n", r * 1000 + $1, $2 }' "$TERMS"; done > "$q"
    local into=""; [ -n "${BULK_INTO:-}" ] && into="--into $BULK_INTO"     # results into a table of the cluster
    local all n
    IFS=, read -r -a all <<< "$hosts"
    for flags in "" "--count"; do
        local kind=top10; [ -n "$flags" ] && kind=count
        # one node, then one statement per node on 2, 3, ... nodes
        for n in ${BULK_NODES:-1 2 3 5}; do
            [ "$n" -le "${#all[@]}" ] || continue
            local hs; hs=$(IFS=,; echo "${all[*]:0:$n}")
            log "b3 enron_x$x $kind on $n nodes"
            # shellcheck disable=SC2086
            W "b3_enron_x${x}_${kind}${into:+_table}_nodes$n" python3 -I bench/plan_c/bulk.py vfts enron "$q" $flags $VP --split "$n" --hosts "$hs" --repeat "$REPEAT" $into
        done
        # control: as many statements on the first node alone
        n=${#all[@]}
        log "b3 enron_x$x $kind, $n statements on one node"
        # shellcheck disable=SC2086
        W "b3_enron_x${x}_${kind}${into:+_table}_node1_split$n" python3 -I bench/plan_c/bulk.py vfts enron "$q" $flags $VP --split "$n" --hosts "${all[0]}" --repeat "$REPEAT" $into
    done
}

for s in $STEPS; do
    log "== $s"
    "step_$s"
done
log "FINISHED"
