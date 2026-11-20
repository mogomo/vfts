# Helpers for the SQL tests. Source it from a test script.
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD, VSQL_DATABASE.
# Every statement is capped by VTEXT_TEST_TIMEOUT seconds (default 600), so a test never hangs.

TEST_NAME=$(basename "$0" .sh)
FAILS=0
TIMEOUT=${VTEXT_TEST_TIMEOUT:-600}

# q 'SQL'          one statement, unaligned, no header; prints the rows (| between columns)
q() { timeout "$TIMEOUT" vsql -X -A -t -q -v ON_ERROR_STOP=1 -c "$1"; }
# qf FILE          run a file of statements
qf() { timeout "$TIMEOUT" vsql -X -A -t -q -v ON_ERROR_STOP=1 -f "$1"; }

# expect "what" "got" "want"
expect() {
    if [ "$2" == "$3" ]; then echo "  ok   $1"
    else echo "  FAIL $1"; echo "       got:  $(echo "$2" | head -20)"; echo "       want: $(echo "$3" | head -20)"; FAILS=$((FAILS + 1)); fi
}

# expect_error "what" 'SQL' "part of the message"
expect_error() {
    local out
    out=$(timeout "$TIMEOUT" vsql -X -A -t -q -v ON_ERROR_STOP=1 -c "$2" 2>&1) && { echo "  FAIL $1 (no error)"; FAILS=$((FAILS + 1)); return; }
    if echo "$out" | grep -qF -- "$3"; then echo "  ok   $1"
    else echo "  FAIL $1"; echo "       got:  $out"; echo "       want an error containing: $3"; FAILS=$((FAILS + 1)); fi
}

done_test() {
    if [ "$FAILS" -eq 0 ]; then echo "$TEST_NAME: OK"; exit 0; fi
    echo "$TEST_NAME: $FAILS FAILURES"; exit 1
}
