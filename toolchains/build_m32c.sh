#!/bin/bash
# Build binutils, GCC and the simulator for m32c-elf (the panel).
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$HOME/tools/m32c/src}
PREFIX=${PREFIX:-$HOME/tools/m32c}
J=${J:-8}
export PATH=$PREFIX/bin:$PATH
BREW=$(brew --prefix)

fix() {     # folder, fix file
    cd "$1"
    if patch -p0 -R -f -s --dry-run < "$2" > /dev/null 2>&1; then
        echo "already in: $2"
    else
        patch -p0 < "$2"
    fi
}
fix "$SRC/gcc-14.4.0/gcc/config/m32c" "$HERE/m32c-gcc14-fixes.patch"
fix "$SRC/gdb-17.2/sim/m32c" "$HERE/m32c-sim-jmpi20.patch"

mkdir -p "$SRC/build-binutils" "$SRC/build-gcc" "$SRC/build-sim"
cd "$SRC/build-binutils"
../binutils-2.47/configure --target=m32c-elf --prefix=$PREFIX --disable-nls --disable-werror --disable-gdb --disable-sim
make -j$J && make install
echo "binutils done"

cd "$SRC/build-gcc"
../gcc-14.4.0/configure --target=m32c-elf --prefix=$PREFIX --enable-languages=c --without-headers --disable-nls \
  --disable-libssp --disable-libquadmath --disable-libgomp --disable-shared --disable-threads \
  --with-gmp=$BREW --with-mpfr=$BREW --with-mpc=$BREW --with-system-zlib
make -j$J all-gcc && make install-gcc
echo "gcc done"

cd "$SRC/build-sim"
../gdb-17.2/configure --target=m32c-elf --prefix=$PREFIX --disable-gdb --disable-gdbserver --disable-binutils \
  --disable-ld --disable-gas --disable-gprof --disable-nls --disable-werror --enable-sim \
  --with-gmp=$BREW --with-mpfr=$BREW
make -j$J all-sim && make install-sim
echo "m32c toolchain done: $PREFIX/bin/m32c-elf-gcc, m32c-elf-run"
