#!/bin/bash
# A test scene: the game from the title straight into driving, on the virtual clock (--test,
# repeatable), with its own user folder (never the player's saves), screenshots and memory hashes.
#
#   tests/scene.sh [options] [-- more stranglehold arguments]
#     --start roam[:N] | race:E   where (teststart.c; default roam:6, Hollywood)
#     --car ID                    the car (default 0, the Corvette)
#     --mode N                    the lighting mode 0-5 (option mode, teststart.c: 0 afternoon, 1 midday,
#                                 2 sunset, 3 night, 4 dusk/overcast, 5 morning; default the event's)
#     --frames N                  how long (default 900)
#     --res WxH                   resolution (default 1280x720)
#     --shots F,F,...             frames to save as PNG (into the output folder)
#     --pad "F:RT=32767*100,..."  scripted controller (SHOLD_PAD); --keys for the keyboard
#     --hash N                    memory and draw-stream hash every N frames (hash.txt)
#     --ab                        run twice, decompiled functions off and on (option decomp),
#                                 hashing every frame: the first frame they differ, or "same"
#                                 (the drawn-frame step pinned at 1: SHOLD_FRAME_STEP_PIN; the
#                                 options only the rewrites have at original: ab_pins below,
#                                 hud_layout, sun, shadow_distance)
#     --ab-base MODE              the first run's option decomp (default off; core keeps the frame
#                                 and state machine, which two-player scenes need)
#     --out DIR                   output folder (default work/scene)
#     --render FPS                frames drawn at FPS on the virtual clock, the simulation at its
#                                 frame_rate under them (interpolated frames; with --option
#                                 frame_rate=60, say); --pad/--keys/--shots count drawn frames
# Free roam is driving by about frame 300 (well under a second of real time at 60 fps).
set -e
cd "$(dirname "$0")/.."
start=roam:6 car=0 frames=900 res=1280x720 shots= pad= keys= hash= ab= abbase=off out=work/scene render= mode= extra=()
while [ $# -gt 0 ]; do
    case "$1" in
        --start) start=$2; shift 2 ;;
        --car) car=$2; shift 2 ;;
        --mode) mode=$2; shift 2 ;;
        --frames) frames=$2; shift 2 ;;
        --res) res=$2; shift 2 ;;
        --shots) shots=$2; shift 2 ;;
        --pad) pad=$2; shift 2 ;;
        --keys) keys=$2; shift 2 ;;
        --hash) hash=$2; shift 2 ;;
        --ab) ab=1; shift ;;
        --ab-base) abbase=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --render) render=$2; shift 2 ;;
        --) shift; extra=("$@"); break ;;
        *) echo "scene.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
user=work/testuser
# --ab pins: options that change what the rewrites do (runtime/decomp/) where the translation can
# only do the original; both runs get them, after any extra options (also fncheck.c, record.sh)
ab_pins=(
    hud_layout=original         # the HUD's margins (game.c hud_native)
    sun=original                # the light's direction (decomp/shaders.c, shadowmap.c)
    shadow_distance=original    # the shadow map's ranges (decomp/shadowmap.c)
)
# the game's window is hidden (DXVK needs one; nothing appears on screen); SHOLD_SHOW=1 shows it
winflag=--hidden
[ -n "$SHOLD_SHOW" ] && winflag=--window
data=${SHOLD_DATA:-$HOME/.local/share/stranglehold-native}
mkdir -p "$user" "$out"
for d in game xbox xbox_data music; do [ -e "$user/$d" ] || ln -s "$data/$d" "$user/$d"; done

run() {     # run <decomp on|off> <hash file or ""> <log>
    rm -f "$user/save.dat" "$user"/shot_*.png
    local h=() r=() l=()
    [ -n "$2" ] && h=(--hash "$2" --hash-every "${hash:-1}")
    [ -n "$render" ] && r=(--option render_rate="$render")
    [ -n "$mode" ] && r+=(--option mode="$mode")
    # (--ab: the options the rewrites have and the translation doesn't, at original)
    if [ -n "$ab" ]; then for o in "${ab_pins[@]}"; do l+=(--option "$o"); done; fi
    SHOLD_TEST_FPS=${render:-$SHOLD_TEST_FPS} SHOLD_PAD="0:UNPLUG${pad:+,400:PLUG,$pad}" SHOLD_KEYS="$keys" SHOLD_SHOT="$shots" \
        timeout -s KILL 600 build/stranglehold --test --user "$user" $winflag --frames "$frames" \
        --option skip_intro=on --option display=window --option resolution="$res" \
        --option start="$start" --option start_car="$car" --option decomp="$1" \
        "${h[@]}" "${r[@]}" "${extra[@]}" "${l[@]}" > "$3" 2>&1 || true
    if ! grep -aq "frame limit $frames reached" "$3"; then
        echo "scene.sh: the run (decomp $1) did not reach frame $frames: see $3" >&2
        grep -a "FATAL" "$3" | head -3 >&2
        exit 1
    fi
}

if [ -n "$ab" ]; then
    hash=${hash:-1}
    # the rewrites' drawn-frame step at 1 in both runs (game.c frame_step: a deliberate difference
    # from the translation at other frame and render rates, which --ab doesn't compare)
    export SHOLD_FRAME_STEP_PIN=1
    run "$abbase" "$out/hash_off.txt" "$out/off.log"
    run on "$out/hash_on.txt" "$out/on.log"
    paste <(awk '{print $1, $2, $3, $4}' "$out/hash_off.txt") <(awk '{print $2, $3, $4}' "$out/hash_on.txt") |
        awk '$2 != $5 || $3 != $6 { print "differ from frame " $1; f = 1; exit }
             $4 != $7 { print "draws differ from frame " $1; f = 1; exit }
             END { if (!f) print "same over " NR " hashes" }'
    exit 0
fi
run on "${hash:+$out/hash.txt}" "$out/run.log"
for f in "$user"/shot_*.png; do [ -e "$f" ] && mv "$f" "$out/"; done
echo "done: $out"
