# How it works

For people who want to read or build the code. To use the mod you only need the patcher (see the [README](../README.md)).

## The mixer

The DJM-900NXS2 has several processors, and the official update file carries a program for each:

| Part of the update file | Chip | What it does |
| :--- | :--- | :--- |
| `main` | TI AM3352 (ARM) | runs the mixer, talks to the panel and the computer |
| `panel` | Renesas M16C | buttons, lamps and the displays |
| `sh` | Renesas SH7267 (SH-2A) | USB |
| `dsp`, `sdsp` | TI sound processors | the sound and the effects |

The update file is the five programs with a version header, each with a CRC-16, and masked with a fixed XOR pattern. It has no signature.

## What the mod changes

1. **The recorder** runs on the SH7267, in flash the stock program leaves free (`0xF8000-0xFFFFF`, and its variables at `0x118E00`). Two stock pointers are pointed at it: a background callback and the interrupt of the recording feed. It takes the mixer's recording feed (48 kHz, 24 bit, the master output with its effects), keeps up to about 1 MB of it in a ring packed without loss (4 to 9 s of music), and writes it to the stick through its own FAT32 code (`rec_control/src`). The file a recording goes into is made beforehand (`DJMREC.TMP`), so the recording can start at once and writing never has to look for room.
2. **16 bit** is rounded from the feed with dither. At **24 bit**, while a stalled stick has filled three quarters of the ring, the sound that keeps arriving is stored with 16 bits until the ring has room again; each such stretch has a marker named `16BIT` and a line in REC-LOG.TXT.
3. **The main processor** passes the recorder's status to the panel and the panel's commands back: one call and a few bytes of an unused command of its own (`rec_control/main_tunnel.py`).
4. **The panel** shows the REC entry in front of UTILITY's first entry and the recorder's state on the lower display (`rec_control/panel/src`). Four stock calls are redirected to code in two free areas of its flash.
5. **The version** on the start-up screen reads PLUS.

## What the mod never touches

The boot code and the updater of the main processor (`0x00000-0x4FFFF`), of the SH7267 (`0x00000-0x9FFFF`) and of the panel (`0xF0000-0xFFFFF`), and both sound programs stay byte for byte as Pioneer made them. `tools/verify_upd.py` refuses a build that changes anything outside the places above. For the first seconds after power-on, and whenever the update buttons were held, the panel code does nothing, so the mixer's own update mode always works.

## Layout

| Folder | What is in it |
| :--- | :--- |
| `rec_control/src` | the recorder (SH7267) |
| `rec_control/panel` | the panel code (M16C) |
| `rec_control/main_tunnel.py` | the main processor's patch |
| `rec_control/test`, `rec_control/panel/test` | the tests, run in the chips' simulators |
| `tools` | update file reader and writer, the checks, the patcher's maker |
| `patcher` | the patcher for users, as a web page and as a Python file, both made by `tools/make_patcher.py` |
| `toolchains` | how to build the two cross compilers |

## Building

You need your own official DJM-900NXS2 firmware version 2.08 UPD file, which is not in this repository (`firmware/README.md`), the two cross compilers (`toolchains/README.md`), and Python 3.

```sh
python3 tools/djmfw.py unpack firmware/original/DJM-900NXS2_208.upd firmware/unpacked_208
python3 build.py                                  # build/DJM-900NXS2_092.upd
python3 tools/make_patcher.py                     # the two patchers, from that build
```

The build is deterministic: with the compilers made by `toolchains/`, `build.py` gives SHA-256 `754ff5361f0f87b07e00f25f06052a27167d939162cc713eff7ff9942c8c2eec`, the build that was tested on the mixer.

Tests:

```sh
python3 tools/test_verify_upd.py
python3 rec_control/test/test_fat.py
python3 rec_control/test/test_pack.py
python3 rec_control/test/test_stream.py             # long
python3 rec_control/panel/test/test_panel.py
python3 rec_control/panel/test/test_menu.py
```

`rec_control/test/test_main_tunnel.py` runs the main processor's patch in an ARM emulator and needs `capstone` and `unicorn` (`pip install capstone==5.0.9 unicorn==2.1.4`). The menu test draws its screens and needs Pillow.

`rec_control/analyze_wav.py FILE.WAV` reads a recording and the recorder's report in its header.

## Pioneer's material

The work is based on Pioneer's official DJM-900NXS2 firmware 2.08. This repository holds none of it: no update file, no built firmware, no listings and no excerpts of its code. Where a tool has to make sure the stock code is what it expects, it compares hashes. Addresses in the source are positions in that firmware, found by reading it.
