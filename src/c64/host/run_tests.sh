#!/bin/sh
# Runs each tests/NAME.keys through the C64 (typed at the keyboard after it
# boots) and compares the text screen at the end with tests/NAME.out.
# NAME.args, if there, adds options. Without the ROMs every test is skipped.
# With --expected, writes the .out files instead.
#   run_tests.sh path/to/c64 ROMDIR [--expected]
here="$(cd "$(dirname "$0")" && pwd)"
EMU="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
ROMS="$2"
[ "$3" = "--expected" ] && EXPECTED=1
case "$ROMS" in /*) ;; *) ROMS="$(pwd)/$ROMS" ;; esac
"$EMU" "$ROMS" -d 1 -f 0 > /dev/null
if [ $? -eq 3 ]; then
    echo "skipping the ROM tests: the C64 ROMs aren't in $ROMS (set C64ROMS)"
    exit 0
fi
cd "$here/tests" || exit 1
pass=0
fail=0
skip=0
for t in *.keys; do
    name="${t%.keys}"
    args=""
    [ -f "$name.args" ] && args="$(cat "$name.args")"
    eval "set -- $args"
    got="$(mktemp)"
    "$EMU" "$ROMS" -K "$t" "$@" > "$got" 2>/dev/null
    if [ $? -eq 3 ]; then  # a ROM it needs (the 1541's) isn't there
        echo "skip $name (a ROM it needs is missing)"
        skip=$((skip + 1))
    elif [ "$EXPECTED" = 1 ]; then
        cp "$got" "$name.out"
        echo "wrote $name.out"
    elif cmp -s "$got" "$name.out"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $name"
        diff "$name.out" "$got" | head -20
    fi
    rm -f "$got"
done
[ "$EXPECTED" = 1 ] && exit 0
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
