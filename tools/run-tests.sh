#!/bin/sh
# Standalone tests: no installed or running Asterisk required.
set -eu
srcdir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/sccp-tests.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
${CC:-cc} -D_GNU_SOURCE -I"$srcdir/src" -Wall -Wextra -Werror ${TEST_CFLAGS:-} \
    "$srcdir/tools/test_cli_tables.c" "$srcdir/src/sccp_cli_table_data.c" \
    ${TEST_LDFLAGS:-} -o "$test_dir/test_cli_tables"
"$test_dir/test_cli_tables"
${CC:-cc} -D_GNU_SOURCE -I"$srcdir/src" -I"$srcdir/tools" -std=c11 -pthread \
    -Wall -Wextra -Werror ${TEST_CFLAGS:-} "$srcdir/tools/test_threadpool.c" \
    ${TEST_LDFLAGS:-} -o "$test_dir/test_threadpool"
"$test_dir/test_threadpool"
