#!/usr/bin/env bash
# The whole benchmark, start to end: vtext (Vertica) against Elasticsearch and OpenSearch on the
# same machine, with the same data and equivalent analyzers. Every number in README.md and in
# blog/ comes from bench/results/*.jsonl written by this script.
#
#   bench/run_all.sh [step ...]
#
# Steps (default: all, in this order):
#   env        record the machine and the versions          -> bench/results/env.json
#   data       download and prepare the datasets           (bench/data/fetch.sh, prepare.py)
#   engines    install Elasticsearch and OpenSearch        (bench/engines/engines.sh)
#   vtext      load every dataset into Vertica, build the vtext indexes, run the queries
#   vqueries   only the vtext query runs, on the indexes already loaded (not in the default list)
#   fenced     the MS MARCO queries again with the functions FENCED, then back to unfenced
#   es         Elasticsearch: load every dataset, run the queries
#   os         OpenSearch: the same
#   refresh    vtext: refresh after 1% new rows (MS MARCO)
#   throughput 32 clients and 1 client through one native HTTP load generator (tools/httpload):
#              vtext through tools/vtext_broker (tserve, batches, one statement per query),
#              Elasticsearch, OpenSearch; 3 rounds in alternating order, CPU governor
#              "performance" while it runs (bench/plan_c/stage1.sh f1)  -> bench/results/throughput.*
#   report     tables and charts                           (bench/report.py)
#
# Requirements: a Vertica node with vtext deployed (make deploy) and vsql connection variables set
# (VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD, VSQL_DATABASE); python3; curl; about 40 GB of
# free disk and 32 GB of RAM. The engines run one at a time: the others are stopped.
# Every step is capped in time (timeout) so a stalled step never blocks the run.
set -uo pipefail
cd "$(dirname "$0")/.."
R=bench/results
OUT=bench/out
mkdir -p "$R" "$OUT"
DATA=${VTEXT_DATA:-$HOME/vtext_data}
SETS=${VTEXT_SETS:-msmarco enron pubmed wiki_he wiki_ar}
REPEAT=${VTEXT_REPEAT:-3}
STEPS=${*:-env data engines vtext fenced es os refresh throughput report}
log() { echo "$(date +%T) $*"; }

queries_of() {   # dataset -> "file throughput_file extra_args"
    case $1 in
        msmarco) echo "$DATA/msmarco/queries.dev.small.tsv $DATA/msmarco/queries.dev.small.tsv --plain" ;;
        *) echo "bench/queries/$1.tsv bench/queries/$1_x25.tsv" ;;
    esac
}
mode_of() { case $1 in wiki_he|wiki_ar) echo exact ;; *) echo stem ;; esac; }   # the engines have no equivalent Hebrew / Arabic stemmer
lang_of() { case $1 in wiki_he) echo he ;; wiki_ar) echo ar ;; *) echo en ;; esac; }
# Wikipedia articles quote Chinese / Japanese words: index them unstemmed, as the ICU tokenizer of the engines does
opts_of() { case $1 in wiki_he|wiki_ar) echo "lang=$(lang_of "$1") on_unsupported=plain" ;; *) echo "lang=$(lang_of "$1")" ;; esac; }

step_env() {
    python3 - > "$R/env.json" <<EOF
import json, os, platform, subprocess
def sh(c):
    try: return subprocess.run(c, shell=True, capture_output=True, text=True, timeout=60).stdout.strip()
    except Exception: return ""
print(json.dumps({
  "cpu": sh("lscpu | grep 'Model name' | cut -d: -f2").strip(), "cores": os.cpu_count(),
  "memory_gb": round(int(sh("grep MemTotal /proc/meminfo | awk '{print \$2}'") or 0) / 1048576, 1),
  "os": sh("grep PRETTY_NAME /etc/os-release | cut -d= -f2").strip('"'), "kernel": platform.release(),
  "vertica": sh("vsql -X -At -c 'SELECT version()'"),
  "vtext": sh("vsql -X -At -c 'SELECT version || CHR(32) || build FROM (SELECT vtext.tversion() OVER()) v'"),
  "elasticsearch": os.environ.get("ES_VERSION", "9.5.6"), "opensearch": os.environ.get("OS_VERSION", "3.10.0"),
  "engine_heap": os.environ.get("ENGINE_HEAP", "16g")}))
EOF
    cat "$R/env.json"
}

step_data() {
    timeout 30000 bench/data/fetch.sh
    timeout 20000 python3 -I bench/data/prepare.py $SETS
}

step_engines() { timeout 3600 bench/engines/engines.sh install; }

run_vt() {   # dataset clients view run_file times_file
    local ds=$1 c=$2 view=$3 run=$4 times=$5
    read -r qf qx extra <<< "$(queries_of "$ds")"
    local q=$qf; [ "$c" -gt 1 ] && q=$qx
    timeout 7200 python3 -I bench/run_vertica.py "$ds" bench "$q" --clients "$c" --repeat "$REPEAT" --mode "$(mode_of "$ds")" \
        --view "$view" --param max_expansions=100000 ${extra:-} ${run:+--run "$run"} ${times:+--times "$times"} | tee -a "$R/vtext_query.jsonl"
}

run_eng() {   # engine dataset clients run_file
    local e=$1 ds=$2 c=$3 run=$4
    read -r qf qx extra <<< "$(queries_of "$ds")"
    local q=$qf; [ "$c" -gt 1 ] && q=$qx
    timeout 7200 python3 -I bench/run_engines.py "$e" "$ds" "$q" --clients "$c" --repeat "$REPEAT" --mode "$(mode_of "$ds")" \
        --lang "$(lang_of "$ds")" ${extra:-} ${run:+--run "$run" --times "${run%.run}.times"} | tee -a "$R/${e}_query.jsonl"
}

step_vtext() {
    bench/engines/engines.sh stop all
    for ds in $SETS; do
        log "vtext: load $ds"
        timeout 10000 bench/load_vertica.sh "$ds" "$(opts_of "$ds")" | tee -a "$R/vtext_load.jsonl"
        vqueries_of "$ds"
    done
}

vqueries_of() {   # the query runs of one dataset (index loaded)
    local ds=$1
    log "vtext: queries $ds"
    # live: every committed row is searched (the default); snap: the rows up to the last refresh,
    # the guarantee of Elasticsearch and OpenSearch (searchable after a refresh)
    run_vt "$ds" 1 live "$OUT/vtext_$ds.run" "$OUT/vtext_$ds.times"
    run_vt "$ds" 1 snap "" "$OUT/vtext_${ds}_snap.times"
    # the answers on the scale of Elasticsearch (bm25='classic'), to compare them with its own
    read -r qf qx extra <<< "$(queries_of "$ds")"
    timeout 3600 python3 -I bench/run_vertica.py "$ds" bench "$qf" --repeat 1 --warmup 0 --mode "$(mode_of "$ds")" \
        --param max_expansions=100000 --param "bm25='classic'" ${extra:-} --run "$OUT/vtext_${ds}_classic.run" > /dev/null
    run_vt "$ds" 32 live "" ""
    run_vt "$ds" 32 snap "" ""
    [ "$ds" != msmarco ] && timeout 3600 python3 -I bench/run_vertica.py "$ds" bench "$qf" --count --repeat 1 --warmup 0 \
        --mode "$(mode_of "$ds")" --param max_expansions=100000 --run "$OUT/vtext_${ds}_count.run" > /dev/null
}

# the vtext query runs again, on the indexes loaded by the step vtext (after a change of the library)
step_vqueries() {
    bench/engines/engines.sh stop all
    for ds in $SETS; do vqueries_of "$ds"; done
}

step_fenced() {
    timeout 600 scripts/deploy.sh --fenced=yes > /dev/null
    read -r qf qx extra <<< "$(queries_of msmarco)"
    timeout 7200 python3 -I bench/run_vertica.py msmarco bench "$qf" --clients 1 --repeat "$REPEAT" $extra \
        | sed 's/"engine": "vtext"/"engine": "vtext", "fenced": true/' | tee -a "$R/vtext_query.jsonl"
    timeout 600 scripts/deploy.sh --fenced=no > /dev/null
}

step_engine() {   # es | os
    local e=$1
    bench/engines/engines.sh stop all
    timeout 600 bench/engines/engines.sh start "$e" || return 1
    for ds in $SETS; do
        log "$e: load $ds"
        timeout 10000 python3 -I bench/load_engines.py "$e" "$ds" --shards 1 | tee -a "$R/${e}_load.jsonl"
        log "$e: queries $ds"
        run_eng "$e" "$ds" 1 "$OUT/${e}_$ds.run"
        run_eng "$e" "$ds" 32 ""
        read -r qf qx extra <<< "$(queries_of "$ds")"
        [ "$ds" != msmarco ] && timeout 3600 python3 -I bench/run_engines.py "$e" "$ds" "$qf" --count --repeat 1 --warmup 0 \
            --mode "$(mode_of "$ds")" --lang "$(lang_of "$ds")" --run "$OUT/${e}_${ds}_count.run" > /dev/null
    done
    bench/engines/engines.sh stop "$e"
}

step_refresh() {
    # the engines first: the vtext test adds the rows to the source table (the count of new rows is
    # 1% of the table before)
    for e in es os; do
        bench/engines/engines.sh stop all
        timeout 600 bench/engines/engines.sh start "$e" > /dev/null
        timeout 7200 bench/refresh_test.sh msmarco "$e" | tee -a "$R/vtext_refresh.jsonl"
    done
    bench/engines/engines.sh stop all
    timeout 7200 bench/refresh_test.sh msmarco | tee -a "$R/vtext_refresh.jsonl"
}

step_throughput() {   # needs the indexes of step vtext and the engines' indexes of steps es and os
    timeout 900 make tools > /dev/null || return 1
    VTEXT_DATA=$DATA F1_SETS="$SETS" timeout 10000 bench/plan_c/stage1.sh f1
    grep '"test": "f1' bench/out/plan_c/results.jsonl > "$R/throughput.jsonl"
    python3 -I bench/plan_c/f1_report.py "$R/throughput.jsonl" --json "$R/throughput.json" | tee "$R/throughput.md"
}

step_report() { python3 -I bench/report.py; }

for s in $STEPS; do
    log "== $s"
    case $s in
        env) step_env ;;
        data) step_data ;;
        engines) step_engines ;;
        vtext) step_vtext ;;
        vqueries) step_vqueries ;;
        fenced) step_fenced ;;
        es) step_engine es ;;
        os) step_engine os ;;
        refresh) step_refresh ;;
        report) step_report ;;
        *) echo "unknown step $s" >&2; exit 2 ;;
    esac
done
log "done"
