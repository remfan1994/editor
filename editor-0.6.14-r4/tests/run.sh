#!/bin/sh
set -eu
[ "$#" -eq 1 ] || { echo 'Usage: tests/run.sh /path/to/commented-editor-0.6.14.c' >&2; exit 2; }
case $1 in /*) baseline=$1;; *) baseline=$PWD/$1;; esac
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
CC=${CC:-cc}
flags='-std=c99 -O2 -Wall -Wextra -Werror -pedantic'
$CC $flags "$baseline" -o "$work/baseline"
for mode in normal restricted; do
    define=
    [ "$mode" != restricted ] || define=-DEDITOR_NO_CMD
    $CC $flags $define editor.c -o "$work/$mode"
    $CC $flags $define tests/rollback-core.c -o "$work/core-$mode"
    mkdir -p "$work/$mode-home/config" "$work/$mode-home/state"
    (cd "$work/$mode-home" && HOME="$PWD" XDG_CONFIG_HOME="$PWD/config" XDG_STATE_HOME="$PWD/state" "$work/core-$mode")
done
python3 tests/rollback-pty.py "$work/normal" "$work/restricted" "$work/baseline"
