#!/bin/sh
# Builds the text-to-speech engine for the PC, runs its checks, and writes
# WAV files of its speech (say_*.wav) into the folder given (default: here).
#   tools/saytest/saytest.sh [out-folder]
set -e
cd "$(dirname "$0")"
OUT=$(cd "${1:-.}" && pwd)
T=$(mktemp -d)
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined saytest.c -lm -o "$T/saytest"
"$T/saytest" "$OUT"
rm -rf "$T"
