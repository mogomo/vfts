#!/usr/bin/env bash
# Install or remove the vtext library, functions and procedures.
#
#   scripts/deploy.sh [--fenced=yes|no] [--undeploy [--drop]] [--echo_only]
#
#   --fenced=no     every function runs inside the Vertica process (default: fastest; a crash in
#                   the library is a crash of the node)
#   --fenced=yes    every function runs in a separate fenced process (about 10% slower, safer)
#   --undeploy      remove the library, its functions and procedures; --drop also drops every
#                   index (its views and node caches) and then the schema vtext
#
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD and VSQL_DATABASE from
# the environment. Nothing is stored here. Run it on a Vertica node: CREATE LIBRARY reads the
# .so from the initiator node's file system (as the database's OS user) and copies it to the other
# nodes; the .so is first copied to a temporary file every user can read.
set -euo pipefail
cd "$(dirname "$0")/.."

FENCED=no
UNDEPLOY=no
DROP=no
ECHO_ONLY=no
for arg in "$@"; do
    case "$arg" in
        --fenced=yes|--fenced=no) FENCED="${arg#--fenced=}" ;;
        --undeploy) UNDEPLOY=yes ;;
        --drop) DROP=yes ;;
        --echo_only) ECHO_ONLY=yes ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "deploy.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

LIBFILE="$PWD/build/libvtext.so"
FENCED_SQL="NOT FENCED"
[ "$FENCED" = yes ] && FENCED_SQL="FENCED"

# the database's OS user must be able to read the library: a copy under /tmp, readable by all
SHARED="/tmp/vtext-deploy-$$.so"
if [ "$UNDEPLOY" = yes ]; then
    CMD=(vsql -X -v ON_ERROR_STOP=1 -f sql/uninstall.sql)
else
    CMD=(vsql -X -v ON_ERROR_STOP=1 -v "libfile='$SHARED'" -v "fenced=$FENCED_SQL" -f sql/install.sql)
fi
if [ "$ECHO_ONLY" = yes ]; then printf '%q ' "${CMD[@]}"; echo; exit 0; fi
if [ "$UNDEPLOY" = no ]; then
    [ -f "$LIBFILE" ] || { echo "deploy.sh: $LIBFILE not found. Run make first." >&2; exit 1; }
    install -m 0644 "$LIBFILE" "$SHARED"
    trap 'rm -f "$SHARED"' EXIT
fi
if [ "$UNDEPLOY" = yes ] && [ "$DROP" = yes ]; then
    # every index first: its partitions, views (in the source table's schema) and node caches
    for nm in $(vsql -X -A -t -q -c "SELECT index_name FROM vtext.indexes" 2> /dev/null); do
        vsql -X -q -c "CALL vtext.drop_index('$nm');" > /dev/null || echo "deploy.sh: drop_index('$nm') failed" >&2
    done
fi
"${CMD[@]}"
if [ "$UNDEPLOY" = yes ] && [ "$DROP" = yes ]; then vsql -X -c "DROP SCHEMA IF EXISTS vtext CASCADE;"; fi
if [ "$UNDEPLOY" = no ]; then
    vsql -X -c "SELECT vtext.tversion() OVER();"
fi
