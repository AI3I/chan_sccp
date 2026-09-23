#!/bin/sh
# Regenerate the checked-in Autotools outputs. Never configure/build implicitly.
set -eu
srcdir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$srcdir"
exec autoreconf --force --install
