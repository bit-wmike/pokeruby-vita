#!/bin/sh
# runs the ARM host build (TARGET=armhost) under qemu-arm: HOSTBIN=tools/port/test/qarm.sh battle.sh ...
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
exec qemu-arm "$REPO/build/port-armhost-sapphire/pokemon_armhost" "$@"
