#!/bin/sh
# Sets up a fresh Ubuntu 24.04 container (e.g. a Claude Code cloud session) for the port:
# 32-bit host build, decomp assets (agbcc), VitaSDK from GitHub releases (vitasdk.org may be
# blocked). Run from the repo root as root. armhost (arm-linux-gnueabihf-gcc-14) is left out:
# its package conflicts with gcc-multilib on Ubuntu.
set -e
REPO=$(pwd)
VITASDK=${VITASDK:-/usr/local/vitasdk}
SDK_TAG=sdk-snapshot-20260825.611.1
SDK_FILE=vitasdk-x86_64-linux-gnu-2026-08-25_12-51-16.tar.bz2
SDK_SHA=ff0e1aa1d968222a98836fcb90ed2cd3e6f5b53074e94d69f8057c718b607a06

apt-get update -q
DEBIAN_FRONTEND=noninteractive apt-get install -y -q gcc-multilib qemu-user binutils-arm-none-eabi \
    libpng-dev build-essential rsync
pip install -q pyelftools pillow

# agbcc + decomp assets (German Sapphire)
W=$(mktemp -d)
git clone -q https://github.com/pret/agbcc "$W/agbcc"
(cd "$W/agbcc" && ./build.sh >/dev/null && ./install.sh "$REPO" >/dev/null)
make clean >/dev/null 2>&1 || true
sh de_before.sh >/dev/null
make sapphire_de CPP=cpp -j"$(nproc)"

# VitaSDK toolchain + libraries
mkdir -p "$VITASDK"
curl -sSLo "$W/$SDK_FILE" "https://github.com/vitasdk/autobuilds/releases/download/$SDK_TAG/$SDK_FILE"
echo "$SDK_SHA  $W/$SDK_FILE" | sha256sum -c
tar xjf "$W/$SDK_FILE" -C "$VITASDK" --strip-components=1
for p in libvita2d zlib libpng libjpeg-turbo; do
    curl -sSLfo "$W/$p.tar.xz" "https://github.com/vitasdk/packages/releases/download/master/$p.tar.xz"
    tar xJf "$W/$p.tar.xz" -C "$VITASDK/arm-vita-eabi"
done
rm -rf "$W"

make -f Makefile.port -j"$(nproc)"
echo "ok. Vita: export VITASDK=$VITASDK PATH=\$VITASDK/bin:\$PATH; make -f Makefile.port TARGET=vita vpk -j$(nproc)"
