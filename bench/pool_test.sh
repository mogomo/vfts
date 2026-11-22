#!/usr/bin/env bash
# Throughput with a Vertica resource pool for search: many small statements at once. The default
# pool (general) plans for few large queries; a search statement needs little memory and one
# execution thread (vfts runs its own threads). The pool is created, the 32-client runs of
# bench/run_all.sh are repeated with SET SESSION RESOURCE_POOL, and the pool is dropped again.
# Results are appended to bench/results/vfts_query.jsonl with the field "session".
#
#   bench/pool_test.sh [dataset ...]          (default: msmarco enron)
#
# Pool settings (environment): POOL_PARALLELISM (EXECUTIONPARALLELISM, default 1),
# POOL_CONCURRENCY (PLANNEDCONCURRENCY, default 64), POOL_MEMORY (MEMORYSIZE, default 4G).
set -uo pipefail
cd "$(dirname "$0")/.."
R=bench/results
DATA=${VFTS_DATA:-$HOME/vfts_data}
SETS=${*:-msmarco enron}
PAR=${POOL_PARALLELISM:-1}
CONC=${POOL_CONCURRENCY:-64}
MEM=${POOL_MEMORY:-4G}
Q() { timeout 600 vsql -X -A -t -q -c "$1"; }
Q "DROP RESOURCE POOL vfts_search" > /dev/null 2>&1
Q "CREATE RESOURCE POOL vfts_search MEMORYSIZE '$MEM' MAXMEMORYSIZE NONE PLANNEDCONCURRENCY $CONC MAXCONCURRENCY NONE
   EXECUTIONPARALLELISM $PAR QUEUETIMEOUT 600 RUNTIMEPRIORITY HIGH" || exit 1
Q "GRANT USAGE ON RESOURCE POOL vfts_search TO $(vsql -X -A -t -c 'SELECT CURRENT_USER()')" > /dev/null 2>&1
SESSION="SET SESSION RESOURCE_POOL = vfts_search"
for ds in $SETS; do
    case $ds in
        msmarco) q=$DATA/msmarco/queries.dev.small.tsv; extra=--plain ;;
        *) q=bench/queries/${ds}_x25.tsv; extra= ;;
    esac
    mode=stem; case $ds in wiki_he|wiki_ar) mode=exact ;; esac
    for view in live snap; do
        echo "$(date +%T) pool: $ds $view"
        timeout 7200 python3 -I bench/run_vertica.py "$ds" bench "$q" --clients 32 --repeat 3 --mode "$mode" --view "$view" \
            --param max_expansions=100000 $extra --session "$SESSION" | tee -a "$R/vfts_query.jsonl"
    done
done
Q "DROP RESOURCE POOL vfts_search" > /dev/null
