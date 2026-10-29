# The two cross compilers

| Chip | Target | Installed in |
|---|---|---|
| Panel, Renesas M16C | `m32c-elf` | `~/tools/m32c` |
| Recorder, Renesas SH7267 (SH-2A, big endian) | `sh-elf` | `~/tools/sh` |

Both from the same sources: binutils 2.47, GCC 14.4.0 (the last release with the m32c backend) and GDB 17.2 for its simulators (`m32c-elf-run`, `sh-elf-run`, which the tests use). Built and used on macOS 15 with Apple silicon.

From Homebrew: `brew install gmp mpfr libmpc zstd`.

1. `toolchains/fetch_sources.sh`: downloads the three archives, checks their SHA-256 and unpacks them.
2. `toolchains/build_m32c.sh`: applies the two fix files and builds `m32c-elf`.
3. `toolchains/build_sh.sh`: builds `sh-elf` from the same sources.

The scripts take their folders from `SRC` and `PREFIX` (and `B` for the SH one), with the places above as defaults. The paths must not contain spaces. `libgcc` does not build for either target and is not needed; the SH script says so.

The fix files:

- `m32c-gcc14-fixes.patch`: the m32c backend crashed on every compile, swapped its far byte loads and stores, and wrote `%lo()`/`%hi()` where the assembler wants `%lo16()`/`%hi16()`.
- `m32c-sim-jmpi20.patch`: the m32c simulator cut `JMPI dsp:20[An]` to 16 bits.

With these compilers `python3 build.py` gives the tested build byte for byte (its SHA-256 is in `docs/HOW_IT_WORKS.md`). If it does not, do not put the result on a mixer.

Other tools: Python 3.8 or later; for `rec_control/test/test_main_tunnel.py` a virtual environment in `~/tools/pyarm` with `capstone` 5.0.9 and `unicorn` 2.1.4; Pillow for the menu test's pictures. `rec_control/test/test_fat.py` makes and checks FAT32 disk images with macOS's own `newfs_msdos`, `fsck_msdos` and `hdiutil`, and needs about 3 GB of free disk.
