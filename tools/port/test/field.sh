#!/bin/sh
# field.sh name g:n:x:y "keys" [VAR=value ...]
#   warps at frame 1700 (no battle). keys = PKR_INPUT "frame:gbakeys:len,..." added after the
#   title-screen presses (A presses up to frame 1690). GBA key bits: A 1, B 2, SELECT 4, START 8,
#   RIGHT 16, LEFT 32, UP 64, DOWN 128. Frames until F (default 1900); shots S (default every 6 from 1700).
#   Example: F=2200 tools/port/test/field.sh yard 9:0:4:8 "1760:16:120,1900:32:120" PKR_VIEW_LOG=1
. "$(dirname "$0")/common.sh"
n=$1; w=$2; keys=$3; shift 3
mkdir -p "$T/$n"; cp "$SAV" "$T/$n/s.sav"
IN=$(python3 -c "print(','.join('%d:1:3'%f for f in range(1,1690,30)))")
[ -n "$keys" ] && IN="$IN,$keys"
if [ -n "$S" ]; then SHOT="PKR_SHOT=$S"; else SHOT="PKR_SHOT_EVERY=6"; fi
env PKR_NOAUDIO=1 PKR_VIEW=1 PKR_TIME=1700000000 PKR_SAVE="$T/$n/s.sav" PKR_OUT="$T/$n" \
 PKR_TEST_WARP=1700:$w PKR_FRAMES=${F:-1900} $SHOT PKR_INPUT="$IN" "$@" \
 $HOSTBIN >"$T/$n/log" 2>&1
