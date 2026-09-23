#!/bin/sh
# Keep provenance optional and output stable; this never modifies .version.
set -eu
srcdir=$(CDPATH= cd -- "$1" && pwd)
output=$2
revision_tmp=$(mktemp "${output}.XXXXXX")
trap 'rm -f "$revision_tmp"' EXIT HUP INT TERM
if test -e "$srcdir/.git"; then
  (cd "$srcdir" && sh tools/autorevision -t h) > "$revision_tmp"
else
  printf '/* Source archive: no Git provenance available. */\n' > "$revision_tmp"
fi
if ! cmp -s "$revision_tmp" "$output"; then
  mv "$revision_tmp" "$output"
fi
