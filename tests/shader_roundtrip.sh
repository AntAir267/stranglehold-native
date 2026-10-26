#!/bin/bash
# Every shader of the game's vshaders.pak and pshaders.pak (66 vs_2_0, 57 ps_1_1, 10 ps_2_0)
# disassembled by tools/d3dasm.py and assembled again must give the same bytes, with no
# instruction left as raw tokens. Also assembles the replacements in data/shaders.
#   tests/shader_roundtrip.sh [game folder]   (default ~/.local/share/stranglehold-native/game)
set -e
cd "$(dirname "$0")/.."
python3 tools/d3dasm.py selftest ${1:+"$1"}
python3 tools/d3dasm.py ctable data/shaders /dev/null && echo "data/shaders: assembles"
