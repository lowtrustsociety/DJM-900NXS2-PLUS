#!/bin/bash
# Build binutils, GCC and the simulator for sh-elf (the recorder).
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
set -e
SRC=${SRC:-$HOME/tools/m32c/src}
PREFIX=${PREFIX:-$HOME/tools/sh}
B=${B:-$PREFIX/build}
J=${J:-8}
export PATH=$PREFIX/bin:$PATH
BREW=$(brew --prefix)
mkdir -p $B/binutils $B/gcc $B/sim
cd $B/binutils
$SRC/binutils-2.47/configure --target=sh-elf --prefix=$PREFIX --disable-nls --disable-werror --disable-gdb --disable-sim > configure.log 2>&1
make -j$J > build.log 2>&1 && make install > install.log 2>&1
echo binutils done
cd $B/gcc
$SRC/gcc-14.4.0/configure --target=sh-elf --prefix=$PREFIX --enable-languages=c --without-headers --with-newlib \
  --disable-nls --disable-shared --disable-threads --disable-libssp --disable-libquadmath --disable-libgomp \
  --with-multilib-list=m2a,m2a-nofpu --with-gmp=$BREW --with-mpfr=$BREW --with-mpc=$BREW --with-system-zlib > configure.log 2>&1
make -j$J all-gcc > build.log 2>&1 && make install-gcc > install.log 2>&1
echo gcc done
make -j$J all-target-libgcc > libgcc.log 2>&1 && make install-target-libgcc >> libgcc.log 2>&1 && echo libgcc done || echo "libgcc FAILED (expected, not needed)"
cd $B/sim
$SRC/gdb-17.2/configure --target=sh-elf --prefix=$PREFIX --disable-nls --disable-werror --disable-gdb --enable-sim \
  --disable-binutils --disable-gas --disable-ld --disable-gprof > configure.log 2>&1
make -j$J all-sim > build.log 2>&1 && make install-sim > install.log 2>&1
echo "sh toolchain done: $PREFIX/bin/sh-elf-gcc, sh-elf-run"
