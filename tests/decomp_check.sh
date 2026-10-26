#!/bin/bash
# The decompiled functions (runtime/decomp/) against the translation, in the real game: each scene
# runs with them off and on and the memory and everything drawn are compared (tests/scene.sh --ab).
# The merge-time check, once per finished piece (in a hidden window): while working, the offline
# check (tests/offline_check.sh) is the one to use.
#   tests/decomp_check.sh [--quick]
set -e
cd "$(dirname "$0")/.."
frames=900; story=1600
[ "$1" = "--quick" ] && { frames=400; story=900; }
fail=0
check() {    # check <name> <scene.sh arguments...>
    local name=$1; shift
    local r
    r=$(tests/scene.sh --ab --out "work/decomp_check/$name" "$@" 2>&1 | tail -1)
    printf '%-14s %s\n' "$name" "$r"
    case "$r" in same*) ;; *) fail=1 ;; esac
}
check roam --frames $frames --hash 1 --pad "420:RT=32767*300,600:LX=20000*60,700:LB*5"
check beach --start roam:2 --frames $frames --hash 2 --pad "420:RT=32767*300"
check roam3 --start roam:3 --frames $frames --hash 2 --pad "420:RT=32767*300"
check race --start race:0x69 --frames $frames --hash 2 --pad "420:RT=32767*400"
check split --start mp:0x9e --frames $frames --hash 2 --ab-base core \
    --pad "420:RT=32767*400,420:P2.RT=32767*300,600:P2.LX=-20000*80" -- --option start_car2=0x10
check story --start off --frames $story --hash 10 \
    --keys "260:SPACE,500:RETURN,700:RETURN,900:RETURN,1100:RETURN,1300:RETURN,1500:RETURN"
exit $fail
