#!/bin/sh
# Linux/POSIX focused tests. Optional r1 source enables byte-for-byte frame
# comparison and the same two redraw benchmarks. No production test hooks.
set -eu
[ "$#" -le 1 ] || { echo 'Usage: tests/run-stability.sh [/path/to/r1/editor.c]' >&2; exit 2; }
case ${1:-} in '') prior=;; /*) prior=$1;; *) prior=$PWD/$1;; esac
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
CC=${CC:-cc}
flags='-std=c99 -O2 -Wall -Wextra -Werror -pedantic'
for mode in normal restricted; do
    define=
    [ "$mode" != restricted ] || define=-DEDITOR_NO_CMD
    $CC $flags $define editor.c -o "$work/$mode"
    $CC $flags $define tests/stability-core.c -o "$work/core-$mode"
    mkdir -p "$work/$mode-home/config" "$work/$mode-home/state"
    (cd "$work/$mode-home" && HOME="$PWD" XDG_CONFIG_HOME="$PWD/config" XDG_STATE_HOME="$PWD/state" "$work/core-$mode")
done
if nm -u "$work/restricted" | grep -E '[[:space:]](fork|vfork|exec[a-z]*|system|popen|posix_spawn[a-z]*|waitpid)(@|$)'; then
    echo 'Unexpected process-execution symbol' >&2; exit 1
fi
if [ -n "$prior" ]; then
    $CC $flags "-DEDITOR_SOURCE=\"$prior\"" tests/redraw-bench.c -o "$work/prior-bench"
    $CC $flags tests/redraw-bench.c -o "$work/current-bench"
    "$work/prior-bench" frames > "$work/prior-frames"
    "$work/current-bench" frames > "$work/current-frames"
    cmp "$work/prior-frames" "$work/current-frames"
    for trial in 1 2 3; do
        echo "Trial $trial baseline"; "$work/prior-bench"
        echo "Trial $trial current"; "$work/current-bench"
    done
fi
