#!/usr/bin/env bash
# Plan-C stage 1: measurements for 32-client throughput (one query per request). Nothing in the
# database or in vfts changes, except a resource pool that C6 creates and drops again.
#
#   bench/plan_c/stage1.sh [step ...]      steps: c0 c1 c2 c4 c5 c6 c3 c12 (default: all, this order)
#
# Every result is one JSON line in bench/out/plan_c/results.jsonl, with the CPU used (cpuwrap.py).
# Datasets and query files as in bench/run_all.sh (indexes loaded by its step vfts).
set -uo pipefail
cd "$(dirname "$0")/../.."
O=bench/out/plan_c
mkdir -p "$O/q"
RES=$O/results.jsonl
DATA=${VFTS_DATA:-$HOME/vfts_data}
PYV=${VFTS_VENV_PY:-$HOME/vfts_venv/bin/python}       # python with vertica_python (C5)
STEPS=${*:-c0 c1 c2 c4 c5 c6 c3 c12}
log() { echo "$(date +%T) $*"; }
W() { local t=$1; shift; timeout 3600 python3 -I bench/plan_c/cpuwrap.py "$RES" "$t" -- "$@"; }

qfile() { case $1 in msmarco) echo "$DATA/msmarco/queries.dev.small.tsv" ;; *) echo "bench/queries/$1_x25.tsv" ;; esac; }
lfile() { case $1 in msmarco) echo "$DATA/msmarco/queries.dev.small.tsv" ;; *) echo "bench/queries/$1.tsv" ;; esac; }   # one-client sets
qextra() { case $1 in msmarco) echo "--plain" ;; *) echo "" ;; esac; }
VF() {   # test dataset view clients [run_vertica options...]
    local t=$1 ds=$2 view=$3 c=$4; shift 4
    W "$t" python3 -I bench/run_vertica.py "$ds" bench "$(qfile "$ds")" --clients "$c" --view "$view" --mode stem \
        --param max_expansions=100000 $(qextra "$ds") "$@"
}
# one-word tsearch on Enron, through the snap view or with the view's text inline
TS_SNAP="SELECT /*+LABEL({label})*/ doc_id FROM (SELECT vfts.tsearch(* USING PARAMETERS query='fraud', index='enron', mode='stem', k=10) OVER() FROM bench.enron_snap) r"
TS_LIVE="SELECT /*+LABEL({label})*/ doc_id FROM (SELECT vfts.tsearch(* USING PARAMETERS query='fraud', index='enron', mode='stem', k=10) OVER() FROM bench.enron_live) r"
UDX0="SELECT /*+LABEL({label})*/ version FROM (SELECT vfts.tversion() OVER()) v"
SEL1="SELECT /*+LABEL({label})*/ 1"
inline_snap() {
    local def
    def=$(vsql -X -A -t -c "SELECT view_definition FROM v_catalog.views WHERE table_schema='bench' AND table_name='enron_snap'")
    echo "SELECT /*+LABEL({label})*/ doc_id FROM (SELECT vfts.tsearch(* USING PARAMETERS query='fraud', index='enron', mode='stem', k=10) OVER() FROM ($def) v) r"
}
SQ() { local t=$1 c=$2 sql=$3; shift 3; W "$t" python3 -I bench/plan_c/sqlload.py --sql "$sql" --clients "$c" --n $(( c * 600 > 3000 ? c * 600 : 3000 )) "$@"; }
label_of() { tail -1 "$RES" | python3 -c "import json,sys; print(json.load(sys.stdin).get('label',''))"; }

step_c0() {   # baseline still valid: the 32-client runs of run_all.sh
    for ds in enron msmarco; do for view in snap live; do VF "c0_${ds}_$view" "$ds" "$view" 32 --repeat 3; done; done
}

step_c1() {   # SQL ceiling: SELECT 1, a trivial UDx, a one-word tsearch, at 1..64 sessions
    for c in 1 8 16 32 64; do
        SQ "c1_select1_c$c" "$c" "$SEL1"
        SQ "c1_udx0_c$c" "$c" "$UDX0"
        SQ "c1_tsearch_snap_c$c" "$c" "$TS_SNAP"
        SQ "c1_tsearch_live_c$c" "$c" "$TS_LIVE"
    done
}

step_c2() {   # where a statement's time goes: data collector steps at 1 and 32 sessions; perf at 32
    local inl; inl=$(inline_snap)
    for c in 1 32; do
        for t in select1:"$SEL1" udx0:"$UDX0" tsearch_snap:"$TS_SNAP" tsearch_inline:"$inl" tsearch_live:"$TS_LIVE"; do
            SQ "c2_${t%%:*}_c$c" "$c" "${t#*:}" --repeat 1 --warmup 0 --n 2000 > /dev/null
            python3 -I bench/plan_c/phases.py "$(label_of)" "c2_${t%%:*}_c$c" | tee -a "$RES"
        done
    done
    # CPU profile of the server during 32 sessions of one-word tsearch, and its off-CPU waits
    local pid; pid=$(pgrep -f "/opt/vertica/bin/vertica -D" | head -1)
    python3 -I bench/plan_c/sqlload.py --sql "$TS_SNAP" --clients 32 --n 60000 --repeat 1 --warmup 0 > "$O/c2_perf_load.json" &
    local lp=$!
    sleep 3
    timeout 60 sudo perf record -F 499 -g -p "$pid" -o "$O/c2_perf.data" -- sleep 10 2> /dev/null
    timeout 60 sudo perf record -e sched:sched_switch -g -p "$pid" -o "$O/c2_sched.data" -- sleep 3 2> /dev/null
    wait $lp
    sudo chown "$USER" "$O"/c2_*.data
    timeout 600 perf report -i "$O/c2_perf.data" --no-children --sort dso --stdio 2> /dev/null | grep -v '^#' | grep -v '^$' | head -30 > "$O/c2_perf_dso.txt"
    timeout 600 perf report -i "$O/c2_perf.data" --no-children --sort dso,symbol --stdio -g none 2> /dev/null | grep -v '^#' | grep -v '^$' | head -80 > "$O/c2_perf_sym.txt"
    timeout 600 perf report -i "$O/c2_sched.data" --no-children --sort symbol --stdio -g caller,0.5,callee --percent-limit 2 2> /dev/null | head -300 > "$O/c2_sched.txt"
    log "c2 perf: $O/c2_perf_dso.txt $O/c2_perf_sym.txt $O/c2_sched.txt"
}

step_c4() {   # engine-only ceiling: vfts_cli with C concurrent searchers on the node cache
    for ds in enron pubmed msmarco; do
        for c in 1 4 8 16 32 64; do
            W "c4_${ds}_c${c}_t1" build/tools/vfts_cli search /tmp/vfts "$ds" "$(qfile "$ds")" --clients "$c" --threads 1 \
                --mode stem --max-expansions 100000 --repeat 3 $(qextra "$ds")
        done
        W "c4_${ds}_c32_t$(nproc)" build/tools/vfts_cli search /tmp/vfts "$ds" "$(qfile "$ds")" --clients 32 --threads "$(nproc)" \
            --mode stem --max-expansions 100000 --repeat 3 $(qextra "$ds")
    done
}

step_c5() {   # client and session cost
    SQ c5_select1_connect 32 "$SEL1" --client vsql_connect --n 3000 --repeat 1
    SQ c5_tsearch_connect 32 "$TS_SNAP" --client vsql_connect --n 3000 --repeat 1
    if [ -x "$PYV" ] && "$PYV" -c "import vertica_python" 2> /dev/null; then
        for k in python python_prepared; do
            W "c5_select1_$k" "$PYV" -I bench/plan_c/sqlload.py --sql "$SEL1" --clients 32 --n 20000 --client "$k"
            W "c5_tsearch_$k" "$PYV" -I bench/plan_c/sqlload.py --sql "$TS_SNAP" --clients 32 --n 20000 --client "$k"
            W "c5_select1_c1_$k" "$PYV" -I bench/plan_c/sqlload.py --sql "$SEL1" --clients 1 --n 3000 --client "$k"
            W "c5_tsearch_c1_$k" "$PYV" -I bench/plan_c/sqlload.py --sql "$TS_SNAP" --clients 1 --n 3000 --client "$k"
            # the same text again and again: prepared once per session, then executed; the data
            # collector shows whether Vertica plans every execution again
            for c in 1 32; do
                W "c5_tsearch_same_c${c}_$k" "$PYV" -I bench/plan_c/sqlload.py --sql "$TS_SNAP" --clients $c --n $((c * 300)) \
                    --client "$k" --same-text --repeat 1 --warmup 0
                python3 -I bench/plan_c/phases.py "$(label_of)" "c5_tsearch_same_c${c}_$k" | tee -a "$RES"
            done
        done
    else
        log "c5: no vertica_python at $PYV, python client runs skipped"
    fi
}

step_c6() {   # sessions, threads parameter, resource pool
    for ds in enron msmarco; do
        VF "c6_${ds}_snap_c64" "$ds" snap 64 --repeat 3
        for th in 1 2 4; do VF "c6_${ds}_snap_c32_threads$th" "$ds" snap 32 --repeat 3 --param "threads=$th"; done
    done
    local me; me=$(vsql -X -A -t -c "SELECT CURRENT_USER()")
    for par in 1 4 AUTO; do
        vsql -X -A -t -q -c "DROP RESOURCE POOL vfts_c6" > /dev/null 2>&1
        vsql -X -A -t -q -c "CREATE RESOURCE POOL vfts_c6 MEMORYSIZE '4G' MAXMEMORYSIZE NONE PLANNEDCONCURRENCY 64 MAXCONCURRENCY NONE
            EXECUTIONPARALLELISM $par QUEUETIMEOUT 600 RUNTIMEPRIORITY HIGH" || continue
        vsql -X -A -t -q -c "GRANT USAGE ON RESOURCE POOL vfts_c6 TO $me" > /dev/null 2>&1
        for ds in enron msmarco; do
            VF "c6_${ds}_snap_c32_pool_par$par" "$ds" snap 32 --repeat 3 --session "SET SESSION RESOURCE_POOL = vfts_c6"
        done
        SQ "c6_tsearch1_c32_pool_par$par" 32 "$TS_SNAP" --session "SET SESSION RESOURCE_POOL = vfts_c6"
    done
    vsql -X -A -t -q -c "DROP RESOURCE POOL vfts_c6" > /dev/null 2>&1
}

repeat_file() {   # base-file lines -> file with at least N lines (the base cycled)
    local base=$1 n=$2 out=$3
    python3 -I - "$base" "$n" "$out" <<'EOF'
import sys
lines = [l for l in open(sys.argv[1], encoding="utf-8") if "\t" in l]
n = max(len(lines), int(sys.argv[2]))
with open(sys.argv[3], "w", encoding="utf-8") as f:
    for i in range(n):
        f.write(f"{i}\t{lines[i % len(lines)].split(chr(9), 1)[1]}")
EOF
}

step_c3() {   # batch-size curve: statements of B queries (parameter queries), C sessions
    for ds in enron msmarco pubmed; do
        for view in snap live; do
            local first=1
            for c in 4 8 16 32; do
                for b in 1 2 5 10 20 50 100 200 500; do
                    local base; base=$(qfile "$ds")
                    local need=$(( c * b * 3 ))
                    local qf=$O/q/${ds}_$need.tsv
                    [ -f "$qf" ] || repeat_file "$base" "$need" "$qf"
                    local wu=0; [ $first = 1 ] && wu=1; first=0
                    W "c3_${ds}_${view}_c${c}_b$b" python3 -I bench/run_vertica.py "$ds" bench "$qf" --clients "$c" --view "$view" \
                        --mode stem --param max_expansions=100000 $(qextra "$ds") --batch "$b" --repeat 3 --warmup "$wu"
                done
            done
        done
    done
}

step_d1() {   # batches with at most 32 queries in flight (sessions x batch <= 32), after parallel batches
    local tag=${D1_TAG:-d1}
    for ds in ${D1_SETS:-enron pubmed msmarco}; do
        for cb in 32:1 16:2 8:4 4:8 2:16 1:32; do
            local c=${cb%%:*} b=${cb#*:}
            local need=$(( c * b * 40 > 3000 ? c * b * 40 : 3000 ))
            local qf=$O/q/${ds}_$need.tsv
            [ -f "$qf" ] || repeat_file "$(qfile "$ds")" "$need" "$qf"
            W "${tag}_${ds}_snap_c${c}_b$b" python3 -I bench/run_vertica.py "$ds" bench "$qf" --clients "$c" --view snap \
                --mode stem --param max_expansions=100000 $(qextra "$ds") --batch "$b" --repeat 3 --warmup 1 ${D1_EXTRA:-}
        done
    done
}

step_e1() {   # Elasticsearch / OpenSearch at 32 clients: native client (tools/httpload) and the Python client
    timeout 900 make tools > /dev/null || return 1
    for e in ${E1_ENGINES:-es os}; do
        local port=9200; [ "$e" = os ] && port=9201
        bench/engines/engines.sh stop all > /dev/null 2>&1
        timeout 600 bench/engines/engines.sh start "$e" > /dev/null || { log "e1: $e did not start"; continue; }
        for ds in ${E1_SETS:-enron pubmed msmarco wiki_he wiki_ar}; do
            local qf; qf=$(qfile "$ds")
            timeout 1800 python3 -I bench/run_engines.py "$e" "$ds" "$qf" --mode "$(modeof "$ds")" --lang "$(langof "$ds")" $(qextra "$ds") \
                --dump "$O/e1_${e}_$ds.req" || continue
            W "e1_${e}_${ds}_native_c32" build/tools/httpload "$port" "$O/e1_${e}_$ds.req" --clients 32 --repeat 3
            W "e1_${e}_${ds}_python_c32" python3 -I bench/run_engines.py "$e" "$ds" "$qf" --clients 32 --repeat 3 --mode "$(modeof "$ds")" \
                --lang "$(langof "$ds")" $(qextra "$ds")
        done
        bench/engines/engines.sh stop "$e" > /dev/null
    done
}

cycle() {   # file N out: the lines of file repeated until there are at least N
    python3 -I -c "import sys; l=open(sys.argv[1]).readlines(); n=max(int(sys.argv[2]), len(l)); open(sys.argv[3],'w').writelines(l[i % len(l)] for i in range(n))" "$1" "$2" "$3"
}
broker_params() {   # dataset -> --param options of vfts_broker
    local ds=$1
    echo "--param mode='$(modeof "$ds")' --param max_expansions=100000"
    [ "$ds" = msmarco ] && echo "--param syntax='plain'"
    case $ds in wiki_*) echo "--param lang='$(langof "$ds")'" ;; esac
}
MINREQ=${MINREQ:-20000}
step_v1() {   # vfts through tools/vfts_broker (serve and sql modes) at 32 clients, native client
    timeout 900 make tools > /dev/null || return 1
    bench/engines/engines.sh stop all > /dev/null 2>&1
    local tag=${V1_TAG:-v1}
    for ds in ${V1_SETS:-enron pubmed msmarco wiki_he wiki_ar}; do
        python3 -I bench/plan_c/mkreq.py "$ds" "$(qfile "$ds")" "$O/v1_$ds.req" --min "$MINREQ"
        for m in ${V1_MODES:-serve sql}; do
            local extra="--mode serve"; [ "$m" = sql ] && extra="--mode sql --sessions ${V1_SESSIONS:-8} --max-batch 32"
            pkill -f "vfts_broker --port 9300" 2> /dev/null; sleep 0.5
            # shellcheck disable=SC2046
            build/tools/vfts_broker --port 9300 --schema bench $extra $(broker_params "$ds") > "$O/${tag}_broker_${ds}_$m.log" 2>&1 &
            local bp=$!
            sleep 1
            kill -0 $bp 2> /dev/null || { log "v1: broker did not start: $(cat "$O/${tag}_broker_${ds}_$m.log")"; continue; }
            W "${tag}_${ds}_${m}_c32" timeout 900 build/tools/httpload 9300 "$O/v1_$ds.req" --clients 32 --repeat 3
            kill $bp 2> /dev/null; wait $bp 2> /dev/null
        done
    done
}

step_e2() {   # Elasticsearch / OpenSearch, native client, as many requests as step v1
    timeout 900 make tools > /dev/null || return 1
    for e in ${E1_ENGINES:-es os}; do
        local port=9200; [ "$e" = os ] && port=9201
        bench/engines/engines.sh stop all > /dev/null 2>&1
        timeout 600 bench/engines/engines.sh start "$e" > /dev/null || { log "e2: $e did not start"; continue; }
        for ds in ${E1_SETS:-enron pubmed msmarco wiki_he wiki_ar}; do
            [ -f "$O/e1_${e}_$ds.req" ] || timeout 1800 python3 -I bench/run_engines.py "$e" "$ds" "$(qfile "$ds")" --mode "$(modeof "$ds")" \
                --lang "$(langof "$ds")" $(qextra "$ds") --dump "$O/e1_${e}_$ds.req"
            cycle "$O/e1_${e}_$ds.req" "$MINREQ" "$O/e2_${e}_$ds.req"
            W "e2_${e}_${ds}_native_c32" build/tools/httpload "$port" "$O/e2_${e}_$ds.req" --clients 32 --repeat 3
        done
        bench/engines/engines.sh stop "$e" > /dev/null
    done
}

governor() {   # set the CPU frequency governor of every CPU (performance for the measurements)
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo "$1" | sudo -n tee "$g" > /dev/null 2>&1 || { log "governor: no passwordless sudo, left as it is"; break; }; done
    log "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
}
f1_vfts() {   # round: vfts through the broker in three modes, every dataset
    local r=$1
    bench/engines/engines.sh stop all > /dev/null 2>&1
    for ds in $F1_SETS; do
        [ -f "$O/v1_$ds.req" ] || python3 -I bench/plan_c/mkreq.py "$ds" "$(qfile "$ds")" "$O/v1_$ds.req" --min "$MINREQ"
        python3 -I bench/plan_c/mkreq.py "$ds" "$(lfile "$ds")" "$O/l1_$ds.req"
        for m in serve batch single; do
            local extra
            case $m in
                serve)  extra="--mode serve" ;;
                batch)  extra="--mode sql --sessions 8 --max-batch 32" ;;      # batches of whatever waits
                single) extra="--mode sql --sessions 32 --max-batch 1" ;;      # one statement per query
            esac
            pkill -x vfts_broker 2> /dev/null; sleep 0.5
            # shellcheck disable=SC2046
            build/tools/vfts_broker --port 9300 --schema bench $extra $(broker_params "$ds") > "$O/f1_broker.log" 2>&1 &
            local bp=$!
            sleep 1
            kill -0 $bp 2> /dev/null || { log "f1: broker did not start: $(cat "$O/f1_broker.log")"; continue; }
            W "f1_r${r}_vfts_${m}_$ds" timeout 900 build/tools/httpload 9300 "$O/v1_$ds.req" --clients 32 --repeat 3
            # one client: latency (the set once, not cycled; best time per query over 3 runs)
            [ "$m" != batch ] && W "f1l_r${r}_vfts_${m}_$ds" timeout 900 build/tools/httpload 9300 "$O/l1_$ds.req" --clients 1 --repeat 3 \
                --times "$O/f1l_r${r}_vfts_${m}_$ds.times"
            kill $bp 2> /dev/null; wait $bp 2> /dev/null
        done
    done
}
f1_engine() {   # round: Elasticsearch or OpenSearch, every dataset
    local r=$1 e=$2 port=9200
    [ "$e" = os ] && port=9201
    bench/engines/engines.sh stop all > /dev/null 2>&1
    timeout 600 bench/engines/engines.sh start "$e" > /dev/null || { log "f1: $e did not start"; return; }
    for ds in $F1_SETS; do
        [ -f "$O/e1_${e}_$ds.req" ] || timeout 1800 python3 -I bench/run_engines.py "$e" "$ds" "$(qfile "$ds")" --mode "$(modeof "$ds")" \
            --lang "$(langof "$ds")" $(qextra "$ds") --dump "$O/e1_${e}_$ds.req"
        [ -f "$O/e2_${e}_$ds.req" ] || cycle "$O/e1_${e}_$ds.req" "$MINREQ" "$O/e2_${e}_$ds.req"
        W "f1_r${r}_${e}_native_$ds" timeout 900 build/tools/httpload "$port" "$O/e2_${e}_$ds.req" --clients 32 --repeat 3
        [ -f "$O/l1_${e}_$ds.req" ] || timeout 1800 python3 -I bench/run_engines.py "$e" "$ds" "$(lfile "$ds")" --mode "$(modeof "$ds")" \
            --lang "$(langof "$ds")" $(qextra "$ds") --dump "$O/l1_${e}_$ds.req"
        W "f1l_r${r}_${e}_native_$ds" timeout 900 build/tools/httpload "$port" "$O/l1_${e}_$ds.req" --clients 1 --repeat 3 \
            --times "$O/f1l_r${r}_${e}_native_$ds.times"
    done
    bench/engines/engines.sh stop "$e" > /dev/null
}
step_f1() {   # final 32-client comparison: 3 rounds, engines in a different order each round
    F1_SETS=${F1_SETS:-enron pubmed msmarco wiki_he wiki_ar}
    timeout 900 make tools > /dev/null || return 1
    local old; old=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2> /dev/null || echo none)
    governor performance
    f1_engine 1 es; f1_engine 1 os; f1_vfts 1
    f1_vfts 2; f1_engine 2 os; f1_engine 2 es
    f1_engine 3 os; f1_vfts 3; f1_engine 3 es
    governor "$old"
}

step_f1v() {   # vfts rounds of f1 only (after a change to vfts; the engines' rounds stay as they are)
    F1_SETS=${F1_SETS:-enron pubmed msmarco wiki_he wiki_ar}
    timeout 900 make tools > /dev/null || return 1
    local old; old=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2> /dev/null || echo none)
    governor performance
    f1_vfts 1; f1_vfts 2; f1_vfts 3
    governor "$old"
}

step_s1() {   # statement shapes: which part of a UDx statement costs (scalar vs transform, volatility)
    local F=${VFTS_FENCED_SQL:-NOT FENCED}
    vsql -X -A -t -q -c "CREATE OR REPLACE FUNCTION vfts.tprobe_v AS LANGUAGE 'C++' NAME 'TProbeVolatileFactory' LIBRARY vfts.vfts $F;
        CREATE OR REPLACE FUNCTION vfts.tprobe_s AS LANGUAGE 'C++' NAME 'TProbeStableFactory' LIBRARY vfts.vfts $F;
        CREATE OR REPLACE FUNCTION vfts.tprobe_i AS LANGUAGE 'C++' NAME 'TProbeImmutableFactory' LIBRARY vfts.vfts $F;" || return 1
    vsql -X -c "EXPLAIN SELECT vfts.tprobe_s(1)" > "$O/s1_explain_stable.txt" 2>&1
    vsql -X -c "EXPLAIN SELECT vfts.tprobe_v(1)" > "$O/s1_explain_volatile.txt" 2>&1
    local shapes=(
        "select1|$SEL1"
        "analytic|SELECT /*+LABEL({label})*/ ROW_NUMBER() OVER() FROM dual"
        "scalar_volatile|SELECT /*+LABEL({label})*/ vfts.tprobe_v(1)"
        "scalar_stable|SELECT /*+LABEL({label})*/ vfts.tprobe_s(1)"
        "scalar_immutable|SELECT /*+LABEL({label})*/ vfts.tprobe_i(1)"
        "transform|$UDX0"
        "transform_dual|SELECT /*+LABEL({label})*/ version FROM (SELECT vfts.tversion() OVER() FROM dual) v"
    )
    for c in 1 8 32; do
        for sh in "${shapes[@]}"; do
            SQ "s1_${sh%%|*}_c$c" "$c" "${sh#*|}" --repeat 2
        done
    done
    for sh in "${shapes[@]}"; do
        SQ "s1p_${sh%%|*}_c32" 32 "${sh#*|}" --repeat 1 --warmup 0 --n 2000 > /dev/null
        python3 -I bench/plan_c/phases.py "$(label_of)" "s1p_${sh%%|*}_c32" | tee -a "$RES"
    done
}

step_p1() {   # off-CPU profile of the server during 32 sessions of the trivial transform UDx
    local pid; pid=$(pgrep -f "/opt/vertica/bin/vertica -D" | head -1)
    python3 -I bench/plan_c/sqlload.py --sql "$UDX0" --clients 32 --n 80000 --repeat 1 --warmup 0 > "$O/p1_load.json" &
    local lp=$!
    sleep 3
    timeout 60 sudo perf trace -s -p "$pid" -- sleep 5 > "$O/p1_trace.txt" 2>&1
    timeout 60 sudo perf lock contention -a -b -- sleep 5 > "$O/p1_lock.txt" 2>&1
    timeout 120 sudo perf record -F 499 --call-graph dwarf,16384 -p "$pid" -o "$O/p1_dwarf.data" -- sleep 4 > /dev/null 2>&1
    for i in 1 2 3 4 5 6; do
        timeout 60 sudo gdb -p "$pid" -batch -ex 'thread apply all bt 16' 2> /dev/null; sleep 0.5
    done > "$O/p1_stacks.txt"
    wait $lp
    sudo chown "$USER" "$O"/p1_* 2> /dev/null
    timeout 900 perf report -i "$O/p1_dwarf.data" --no-children --sort dso,symbol --stdio -g caller,0.5,callee --percent-limit 1.5 2> /dev/null | head -400 > "$O/p1_dwarf.txt"
    log "p1: $O/p1_trace.txt p1_lock.txt p1_dwarf.txt p1_stacks.txt"
}

modeof() { case $1 in wiki_he|wiki_ar) echo exact ;; *) echo stem ;; esac; }
langof() { case $1 in wiki_he) echo he ;; wiki_ar) echo ar ;; *) echo en ;; esac; }
step_c12() {   # fuzzy and phrase gaps: the engine alone (no SQL) against Elasticsearch's profile, per kind
    local C12SETS="enron pubmed wiki_he wiki_ar"
    timeout 900 make tools > /dev/null || return 1
    for ds in $C12SETS; do
        timeout 1800 build/tools/vfts_cli search /tmp/vfts "$ds" "bench/queries/$ds.tsv" --threads 1 --repeat 5 --stats \
            --mode "$(modeof "$ds")" --max-expansions 100000 --times "$O/c12_cli_$ds.tsv" | tail -2
    done
    bench/engines/engines.sh stop all > /dev/null 2>&1
    if timeout 600 bench/engines/engines.sh start es > /dev/null; then
        for ds in $C12SETS; do
            timeout 1800 python3 -I bench/plan_c/es_profile.py es "$ds" "bench/queries/$ds.tsv" "$O/c12_es_$ds.tsv" \
                --mode "$(modeof "$ds")" --lang "$(langof "$ds")" --repeat 5
        done
        bench/engines/engines.sh stop es > /dev/null
    fi
    python3 -I bench/plan_c/c12_report.py "$O" $C12SETS 2>> "$RES" | tee "$O/c12_table.md"
}

for s in $STEPS; do
    log "== $s"
    "step_$s"
done
log "FINISHED"
