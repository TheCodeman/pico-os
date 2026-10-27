#!/bin/sh
# Runs each tests/NAME.in through the interpreter and compares the transcript
# with tests/NAME.out. With --expected ROM, writes the .out files from the
# real ROM through a2oracle instead.
out="$(cd "$1" && pwd)"
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here/tests" || exit 1
if [ "$2" = "--expected" ]; then
    for t in *.in; do
        name="${t%.in}"
        if [ -f "$name.dump" ]; then
            "$out/a2oracle" "$3" "$t" -d "$(cat "$name.dump")" "$name.mem" > "$name.out"
        else
            "$out/a2oracle" "$3" "$t" > "$name.out"
        fi
        echo "wrote $name.out"
    done
    exit 0
fi
pass=0
fail=0
work="$(mktemp -d)"
for t in *.in; do
    name="${t%.in}"
    dump=""
    [ -f "$name.dump" ] && dump="-d $(cat "$name.dump") $work/$name.mem"
    (cd "$work" && timeout 20 "$out/applesoft" -i "$here/tests/$t" $dump) > "$work/$name.got" 2>&1
    ok=true
    cmp -s "$work/$name.got" "$name.out" || ok=false
    if [ -n "$dump" ]; then
        start="$(cut -d- -f1 "$name.dump")"
        if ! msg="$("$here/memcmp.py" "$work/$name.mem" "$name.mem" "$start")"; then
            ok=false
            echo "$name: memory: $msg"
        fi
    fi
    if $ok; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $name"
        diff "$name.out" "$work/$name.got" | head -20
    fi
done
rm -rf "$work"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
