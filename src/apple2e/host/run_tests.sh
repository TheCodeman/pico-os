#!/bin/sh
# Runs each tests/NAME.keys through the Apple IIe (typed at the keyboard
# after it boots) and compares the text screen at the end with
# tests/NAME.out. NAME.args, if there, adds options; NAME.setup, if there,
# is run first (exit status 77: skip the test). Both can use $EMU, $ROM, $P5 (the Disk II boot ROM, found
# next to the IIe ROM or given as A2P5), $DISKS (disk images, A2DISKS or
# DISKS/ at the top of the tree) and $WORK (an empty folder). A test whose
# ROM or disk images aren't there is skipped. With --expected, writes the
# .out files instead.
#   run_tests.sh path/to/apple2e ROM [--expected]
here="$(cd "$(dirname "$0")" && pwd)"
top="$(cd "$here/../../.." && pwd)"
EMU="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
ROM="$2"
[ "$3" = "--expected" ] && EXPECTED=1
case "$ROM" in /*) ;; *) ROM="$(pwd)/$ROM" ;; esac
if [ ! -f "$ROM" ]; then
    echo "no ROM at $ROM (set A2ROM)"
    exit 2
fi
P5="${A2P5:-$(ls "$(dirname "$ROM")"/*341-0027* 2>/dev/null | head -1)}"
DISKS="${A2DISKS:-$top/DISKS}"
export EMU ROM P5 DISKS
cd "$here/tests" || exit 1
pass=0
fail=0
skip=0
for t in *.keys; do
    name="${t%.keys}"
    WORK="$(mktemp -d)"
    export WORK
    args=""
    [ -f "$name.args" ] && args="$(cat "$name.args")"
    eval "set -- $args"
    # Skip the test if a ROM or disk it names is missing.
    missing=""
    prev=""
    for a in "$@"; do
        case "$prev" in -P|-1|-2)
            case "$a" in "$WORK"/*) ;; *) [ -f "$a" ] || missing="$a" ;; esac ;;
        esac
        prev="$a"
    done
    if [ -n "$missing" ]; then
        echo "skip $name (no $missing)"
        skip=$((skip + 1))
        rm -rf "$WORK"
        continue
    fi
    if [ -f "$name.setup" ]; then
        sh "./$name.setup"
        status=$?
        if [ "$status" -eq 77 ]; then
            echo "skip $name (setup: something it needs is missing)"
            skip=$((skip + 1))
            rm -rf "$WORK"
            continue
        elif [ "$status" -ne 0 ]; then
            echo "FAIL $name (setup)"
            fail=$((fail + 1))
            rm -rf "$WORK"
            continue
        fi
    fi
    "$EMU" "$ROM" -K "$t" "$@" > "$WORK/got"
    if [ "$EXPECTED" = 1 ]; then
        cp "$WORK/got" "$name.out"
        echo "wrote $name.out"
    elif cmp -s "$WORK/got" "$name.out"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $name"
        diff "$name.out" "$WORK/got" | head -20
    fi
    rm -rf "$WORK"
done
[ "$EXPECTED" = 1 ] && exit 0
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
