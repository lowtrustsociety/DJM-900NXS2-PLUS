# firmware/

Pioneer's official firmware is not in this repository. A build needs one file, from Pioneer DJ's own download page for the DJM-900NXS2, firmware version 2.08 (the download is a zip that holds it):

    firmware/original/DJM-900NXS2_208.upd
    SHA-256 482817cfe5984894db8e198eba8d3dd33e226d523ac5e1555bbccb5cc6c08538

The panel tests read its unpacked images:

    python3 tools/djmfw.py unpack firmware/original/DJM-900NXS2_208.upd firmware/unpacked_208

Everything built from it is Pioneer's firmware with our changes in it and stays out of git (`.gitignore`).
