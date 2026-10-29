#!/bin/bash
# Download and check the sources of the two cross compilers.
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
set -e
SRC=${SRC:-$HOME/tools/m32c/src}
GNU=${GNU:-https://ftp.gnu.org/gnu}
mkdir -p "$SRC"
cd "$SRC"

get() {     # folder on the server, archive, SHA-256
    [ -f "$2" ] || curl -fL --retry 3 -o "$2" "$GNU/$1/$2"
    echo "$3  $2" | shasum -a 256 -c -
    [ -d "${2%.tar.xz}" ] || tar xf "$2"
}

get binutils        binutils-2.47.tar.xz 154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff
get gcc/gcc-14.4.0  gcc-14.4.0.tar.xz    752b6f567beac83159c77a7680b1316bdd784738bff9a9d070112c09da90f6d9
get gdb             gdb-17.2.tar.xz      1c036c0d72e4b3d1fb5c94c88632add6f9d76f4d7c4d2ea793c12a9f19a3228c
echo "sources ready in $SRC"
