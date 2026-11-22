#!/usr/bin/env bash
# vfts: one install for any Vertica database (one node or many, Enterprise or Eon).
#
#   ./install.sh                 check, build, test, deploy (unfenced), build the tools, verify
#   ./install.sh --fenced        the same, the functions fenced (a crash cannot stop a node; slower)
#   ./install.sh --check         only check the environment
#   ./install.sh --skip-tests    no engine unit tests (faster)
#   ./install.sh --uninstall     remove the library and functions (index tables stay)
#   ./install.sh --uninstall --drop    also drop the schema vfts with every index
#
# Run it on a Vertica node, as a user that can read the repository, with the vsql connection
# variables of a superuser (dbadmin):
#   export VSQL_USER=dbadmin VSQL_PASSWORD=... VSQL_DATABASE=...   (VSQL_HOST / VSQL_PORT: this node)
# The library is copied to every node by CREATE LIBRARY: run it once per database. Nothing is stored.
# Every step is capped in time; the first build of the dependencies takes 5 to 20 minutes.
set -uo pipefail
cd "$(dirname "$0")"
export PATH="/opt/vertica/bin:$PATH"

FENCED=no; CHECK_ONLY=no; SKIP_TESTS=no; UNINSTALL=no; DROP=no
for a in "$@"; do
    case "$a" in
        --fenced) FENCED=yes ;;
        --check) CHECK_ONLY=yes ;;
        --skip-tests) SKIP_TESTS=yes ;;
        --uninstall) UNINSTALL=yes ;;
        --drop) DROP=yes ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "install.sh: unknown option $a (see --help)"; exit 2 ;;
    esac
done
[ "$DROP" = yes ] && [ "$UNINSTALL" = no ] && { echo "install.sh: --drop goes with --uninstall"; exit 2; }
[ "$UNINSTALL" = yes ] && { [ "$CHECK_ONLY" = yes ] || [ "$FENCED" = yes ]; } && { echo "install.sh: --uninstall takes only --drop"; exit 2; }

B=$(tput bold 2> /dev/null || true); N=$(tput sgr0 2> /dev/null || true)
step() { echo; echo "${B}== $*${N}"; }
ok()   { echo "   ok    $*"; }
warn() { echo "   note  $*"; }
die()  { echo "   FAIL  $*"; echo; echo "install.sh stopped at this step; the steps after it did not run."; exit 1; }
# --foreground: vsql can ask for a password on the terminal, and Ctrl-C reaches it
q()    { timeout --foreground 120 vsql -X -A -t -q -c "$1" 2> /dev/null; }
# Ctrl-C also stops a long step (make, deploy) that runs under timeout in its own process group
trap 'echo; echo "install.sh interrupted"; pkill -TERM -P $$ 2> /dev/null; exit 130' INT TERM

# ---------------------------------------------------------------- 1. environment
step "1. Environment"
command -v vsql > /dev/null || die "vsql not found (expected in /opt/vertica/bin): run this on a Vertica node"
v=$(q "SELECT version();") || true
[ -n "$v" ] || die "cannot connect with vsql: set VSQL_USER, VSQL_PASSWORD, VSQL_DATABASE (and VSQL_HOST, VSQL_PORT)"
ok "connected: $v"
ver=$(echo "$v" | sed -n 's/.*v\([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')
[ -n "$ver" ] || warn "could not read the version from: $v"
if [ -n "$ver" ] && [ "$(printf '%s\n26.2\n' "$ver" | sort -V | head -1)" != "26.2" ]; then
    die "Vertica $ver: vfts needs 26.2 or later (tested on 26.3)"
fi
[ "$ver" = "26.2" ] && warn "Vertica 26.2: same SDK version as 26.3 (26.2.0), but vfts was tested on 26.3 only"
[ "$(q "SELECT is_super_user FROM v_catalog.users WHERE user_name = CURRENT_USER();")" = t ] || \
    die "the user $(q 'SELECT CURRENT_USER();') is not a superuser: CREATE LIBRARY needs one (dbadmin)"
ok "superuser: $(q 'SELECT CURRENT_USER();')"
# CREATE LIBRARY reads the file on the node the session is connected to: that must be this machine
node_addr=$(q "SELECT node_address FROM v_catalog.nodes WHERE node_name = LOCAL_NODE_NAME();")
[ -n "$node_addr" ] || die "could not read the address of the session's node"
addrs="$(hostname -I 2> /dev/null) $(ip -o addr show 2> /dev/null | awk '{sub(/\/.*/, "", $4); print $4}' | tr '\n' ' ')"
case " $addrs 127.0.0.1 ::1 " in
    *" $node_addr "*) ok "the session's node ($node_addr) is this machine" ;;
    *) die "the session is connected to node $node_addr, not to this machine: set VSQL_HOST to this node (or localhost)" ;;
esac
eon=$(q "SELECT COUNT(DISTINCT subcluster_name) FROM v_catalog.subclusters;" || echo 0); eon=${eon:-0}
nodes=$(q "SELECT COUNT(*) FROM v_catalog.nodes;")
if [ "${eon:-0}" -gt 0 ]; then ok "Eon mode: $nodes nodes in $eon subcluster(s)"; else ok "Enterprise mode: $nodes node(s)"; fi

if [ "$UNINSTALL" = yes ]; then
    step "Uninstall"
    if [ "$DROP" = yes ]; then timeout 600 scripts/deploy.sh --undeploy --drop || die "uninstall failed"
    else timeout 600 scripts/deploy.sh --undeploy || die "uninstall failed"; fi
    ok "removed$( [ "$DROP" = yes ] && echo ", with the schema vfts and every index" || echo "; the index tables stay (--drop removes them)")"
    exit 0
fi

[ -f /opt/vertica/sdk/include/Vertica.h ] || die "the Vertica SDK is missing (/opt/vertica/sdk): it comes with the server package"
ok "SDK: /opt/vertica/sdk"
command -v g++ > /dev/null || die "g++ not found: install g++ 8 or later (RHEL: gcc-c++, Ubuntu: g++)"
gv=$(g++ -dumpversion | cut -d. -f1)
[ "$gv" -ge 8 ] 2> /dev/null || die "g++ $gv is too old: 8 or later is needed"
ok "g++ $(g++ -dumpfullversion 2> /dev/null || g++ -dumpversion)"
for t in make curl tar; do command -v $t > /dev/null || die "$t not found"; done
ok "make, curl, tar"
case "$PWD" in
    *[[:space:]]*) die "the repository's path has a space ($PWD): ICU's build cannot handle that; move it (e.g. to ~/vfts)" ;;
esac
[ "$CHECK_ONLY" = yes ] && { echo; echo "Environment ready."; exit 0; }

# ---------------------------------------------------------------- 2. build
step "2. Build (dependencies once, then vfts)"
mkdir -p build || die "cannot create build/ in $PWD"
[ -f third_party/install/lib/libicuuc.a ] || echo "   building ICU, Snowball and CRoaring once (5 to 20 minutes; log: build/deps.log)"
timeout 3600 make deps > build/deps.log 2>&1 || die "dependencies failed: see build/deps.log"
ok "dependencies (third_party/install: ICU, Snowball, CRoaring)"
J=$(nproc 2> /dev/null || echo 2)
timeout 3600 make -j"$J" > build/make.log 2>&1 || die "build failed: see build/make.log"
ok "build/libvfts.so"
if [ "$SKIP_TESTS" = no ]; then
    timeout 3600 make -j"$J" test > build/test.log 2>&1 || die "engine tests failed: see build/test.log"
    ok "engine unit tests passed"
fi
timeout 3600 make -j"$J" tools > build/tools.log 2>&1 && ok "tools: build/tools/vfts_broker, vfts_cli, httpload" \
    || warn "tools did not build (see build/tools.log); the database functions do not need them"

# ---------------------------------------------------------------- 3. deploy
step "3. Deploy into the database ($( [ "$FENCED" = yes ] && echo fenced || echo unfenced ))"
timeout 900 scripts/deploy.sh --fenced="$FENCED" > build/deploy.log 2>&1 || die "deploy failed: see build/deploy.log"
ok "schema vfts: library, functions, procedures, tables (copied to every node)"

# ---------------------------------------------------------------- 4. verify
step "4. Verify"
tv=$(q "SELECT version || ' (' || build || ', ICU ' || icu || ')' FROM (SELECT vfts.tversion() OVER()) v;")
[ -n "$tv" ] || die "vfts.tversion() did not answer"
ok "vfts $tv"
out=$(EX_SCHEMA=vfts_selftest EX_INDEX=vfts_selftest timeout 600 scripts/examples.sh 2>&1); rc=$?
if [ $rc -ne 0 ] || grep -qE "ERROR|FATAL|could not connect|server closed" <<< "$out"; then
    printf '%s\n' "$out" | grep -B3 -E "ERROR|FATAL|could not connect|server closed" | head -20
    [ $rc -eq 124 ] && die "the examples did not finish in 10 minutes"
    [ $rc -eq 3 ] && die "the schema or index vfts_selftest exists and was not made by vfts: not touched"
    die "the examples failed (exit $rc; above)"
fi
ok "every function ran on a test table (schema vfts_selftest, then removed)"

# ---------------------------------------------------------------- done
step "Installed"
cat << EOF
   Next:
   - Index a table:  CALL vfts.create_index('docs', 'app.docs', 'lang=en');   (README: Prepare a table)
   - Search:         SELECT * FROM (SELECT vfts.tsearch(* USING PARAMETERS index='docs', query='...', k=10) OVER()
                                    FROM app.docs_live) r;
   - Let users search: GRANT USAGE ON SCHEMA app TO <role>; GRANT SELECT ON app.docs_live, app.docs_snap TO <role>;
   - Keep it fresh:  CALL vfts.refresh_index('docs');   on a schedule
   - Node caches:    /tmp/vfts by default; for production pass cache_dir=<dir> to create_index
                     (the directory must exist on every node, writable by the database's OS user)
EOF
if [ "${eon:-0}" -gt 1 ]; then
    echo "   - Eon: after create_index / refresh_index on the primary subcluster, run CALL vfts.load_all('docs');"
    echo "     from a session on each secondary subcluster"
fi
echo "   - Many clients:   build/tools/vfts_broker --port 9300 --schema app --mode serve   (README: tserve and vfts_broker)"
echo "   - More:           scripts/demo.sh (a guided tour), tests/sql/run_all.sh (SQL tests)"
