#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
CC=${CC:-cc}
flags=${CFLAGS:--std=c99 -O2 -Wall -Wextra -Werror -pedantic}
for mode in normal restricted; do
    define=
    [ "$mode" != restricted ] || define=-DEDITOR_NO_CMD
    $CC $flags $define editor.c -o "$work/$mode"
    $CC $flags $define tests/wrap-core.c -o "$work/core-$mode"
    mkdir -p "$work/$mode-home/config" "$work/$mode-home/state"
    echo "$mode:"
    (cd "$work/$mode-home" && HOME="$PWD" XDG_CONFIG_HOME="$PWD/config" XDG_STATE_HOME="$PWD/state" "$work/core-$mode")
done
python3 tests/wrap-pty.py "$work/normal" "$work/restricted"
