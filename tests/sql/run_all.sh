#!/usr/bin/env bash
# Every SQL test against the database vsql connects to (VSQL_* variables). Each test creates and
# drops its own schema and index (vf_*). Stops at the first failing test.
set -uo pipefail
cd "$(dirname "$0")"
for t in test_tokenize test_search test_languages test_freshness test_snippet_hybrid test_serve test_access; do
    ./$t.sh || { echo "FAILED: $t"; exit 1; }
done
echo "All SQL tests passed."
