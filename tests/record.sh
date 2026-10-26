#!/bin/bash
# Recordings for the offline checker: each scene of tests/decomp_check.sh run once (hidden, the
# original code), with whole frames (the frame function 0x45c1b0, simulation and drawing; the
# drawing 0x4d2db0 for two players) captured with their calls into the runtime
# (runtime/capture.c, devrec.c) at two points. They go to
# work/recordings/<scene>/ (~140 MB a frame). tests/offline_check.sh then checks the rewrites
# against them as often as needed, without running the game.
#   tests/record.sh [scene...]        (default: all six, plus the race at 2560x1440, two players side
#                                     by side, a lowrider, a crash and a 120 Hz render rate)
set -e
cd "$(dirname "$0")/.."
out=work/recordings
user=work/testuser
# options that change what the rewrites do where the translation can only do the original: the
# recordings are made with them at original (as tests/scene.sh's ab_pins, fncheck.c)
pins=(
    hud_layout=original         # the HUD's margins, the minimap's viewport included (game.c hud_native)
    sun=original                # the light's direction (decomp/shaders.c, shadowmap.c)
    shadow_distance=original    # the shadow map's ranges (decomp/shadowmap.c)
)
record() {      # record <scene> [fn:]<frames to capture: a,b> <scene.sh arguments...>
    local name=$1 at=$2 fn=45c1b0; shift 2
    [[ $at == *:* ]] && { fn=${at%%:*}; at=${at#*:}; }
    local caps="" last=0
    for f in ${at//,/ }; do caps="$caps${caps:+,}$fn@$f"; [ "$f" -gt "$last" ] && last=$f; done
    rm -rf "$user/captures" "$out/$name"
    mkdir -p "$out/$name"
    # (the options only the rewrites have, at original: pins)
    local a=("$@")
    [[ " ${a[*]} " == *" -- "* ]] || a+=(--)
    for o in "${pins[@]}"; do a+=(--option "$o"); done
    SHOLD_CAPTURE="$caps" tests/scene.sh --frames $((last + 2)) --out "$out/$name" "${a[@]}" > /dev/null
    mv "$user"/captures/*.cap "$user"/captures/*.cap.dev "$out/$name/"
    rm -rf "$user/captures"
    echo "$name: $(ls "$out/$name"/*.cap | wc -l) frames recorded"
}
want() { [ $# -eq 0 ] || [[ " $* " == *" $1 "* ]]; }
scenes="$*"
want roam $scenes && record roam 500,800 --pad "420:RT=32767*300,600:LX=20000*60,700:LB*5" -- --option decomp=off
want beach $scenes && record beach 500,700 --start roam:2 --pad "420:RT=32767*300" -- --option decomp=off
want roam3 $scenes && record roam3 300,600 --start roam:3 --pad "420:RT=32767*300" -- --option decomp=off
want race $scenes && record race 500,800 --start race:0x69 --pad "420:RT=32767*400" -- --option decomp=off
# (the HUD scaled: the race at 2560x1440, one frame)
want race2560 $scenes && record race2560 600 --start race:0x69 --res 2560x1440 --pad "420:RT=32767*400" -- --option decomp=off
# (two players need the frame and state machine's rewrites, option decomp = core, and keep player
# 2's input in the runtime (split.c), which a capture doesn't hold: the drawing only, 0x4d2db0)
want split $scenes && record split 4d2db0:500,700 --start mp:0x9e \
    --pad "420:RT=32767*400,420:P2.RT=32767*300,600:P2.LX=-20000*80" -- --option start_car2=0x10 --option decomp=core
want splitv $scenes && record splitv 4d2db0:500 --start mp:0x9e \
    --pad "420:RT=32767*400,420:P2.RT=32767*300" -- --option start_car2=0x10 --option decomp=core --option split=vertical
# (paths the six scenes miss: the cars' axles, only lowriders (mp:0xae High Bounce, two players);
# the damage textures' swaps after a crash; the cars' pose blended at a 120 Hz render rate)
want lowrider $scenes && record lowrider 4d2db0:500,700 --start mp:0xae \
    --pad "420:RT=32767*400,500:A*60,600:A*30" -- --option decomp=core
want crash $scenes && record crash 4d2db0:900,915,930,960 \
    --pad "420:RT=32767*300,600:LX=20000*60,700:LB*5" -- --option decomp=off
want render120 $scenes && record render120 4d2db0:600 --render 120 \
    --pad "420:RT=32767*300,600:LX=20000*60" -- --option frame_rate=60 --option decomp=off
want story $scenes && record story 700,1500 --start off \
    --keys "260:SPACE,500:RETURN,700:RETURN,900:RETURN,1100:RETURN,1300:RETURN,1500:RETURN" -- --option decomp=off
exit 0
