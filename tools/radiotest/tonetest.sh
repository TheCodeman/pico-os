#!/bin/sh
# The radio's bass and treble on a PC (tonetest.c). An MP3 given goes through
# the decoder, the resampler and the shelves too.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../src"
out=${TMPDIR:-/tmp}/radio_tonetest
mkdir -p "$out"
cc -O2 -Wall -I"$here/stubs" -I"$src/radio" -I"$src/radio/helix" -o "$out/tonetest" "$here/tonetest.c" \
    $(ls "$src"/radio/helix/*.c) -w -lm
cd "$out" && ./tonetest "$@"
