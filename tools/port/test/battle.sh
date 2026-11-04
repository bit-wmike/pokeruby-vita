#!/bin/sh
# battle.sh name g:n:x:y species shots [VAR=value ...]
#   warps at frame 1700, wild battle (species, level 8) at frame 1900, A every 30 frames.
#   Frames until F (default 2400). Pictures: $PKR_TEST_DIR/name/frame_NNNNN.ppm (480x272), log: .../log
#   Example: F=2600 tools/port/test/battle.sh r104 0:19:3:9 25 2450 PKR_TEST_MOVES=57
. "$(dirname "$0")/common.sh"
n=$1; w=$2; sp=$3; shots=$4; shift 4
mkdir -p "$T/$n"; cp "$SAV" "$T/$n/s.sav"
env PKR_NOAUDIO=1 PKR_VIEW=1 PKR_TIME=1700000000 PKR_SAVE="$T/$n/s.sav" PKR_OUT="$T/$n" PKR_AUTO_A=1 \
 PKR_TEST_WARP=1700:$w PKR_TEST_BATTLE=1900:$sp:8 PKR_FRAMES=${F:-2400} PKR_SHOT=$shots "$@" \
 $HOSTBIN >"$T/$n/log" 2>&1
