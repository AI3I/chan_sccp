#!/bin/sh
# Prepare real Asterisk headers in a private prefix; never install into /usr.
set -eu
version=${1:?Asterisk major version required}
workdir=${2:?Absolute build directory required}
case "$version" in 20|21|22|23|24) ;; *) echo 'Expected Asterisk 20–24' >&2; exit 1;; esac
case "$workdir" in /*) ;; *) echo 'Build directory must be absolute' >&2; exit 1;; esac
mkdir -p "$workdir"
git clone --depth 1 --branch "$version" https://github.com/asterisk/asterisk.git "$workdir/source"
cd "$workdir/source"
git rev-parse HEAD > "$workdir/asterisk-revision.txt"
./configure --prefix="$workdir/prefix" --without-pjproject --without-jansson-bundled
make -j2 include/asterisk/buildopts.h
make install-headers
