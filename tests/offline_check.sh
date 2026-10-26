#!/bin/bash
# The rewrites (runtime/decomp/) against the original, offline: every recording of tests/record.sh
# replayed by the function checker (tests/fncheck.c --capture FILE --ab), the translated code and
# the rewrites from the same captured frame, with the recorded results of every call into the
# runtime (Direct3D included) and the stack-leftover fixes in both; memory, registers and the
# draw-stream hash compared. No game runs. Each frame takes seconds.
#   tests/offline_check.sh [scene...]          (SHOLD_DECOMP_OFF=addr,... switches rewrites off)
cd "$(dirname "$0")/.."
make -s build/fncheck JOBS=4 >/dev/null || exit 1
fail=0 n=0
for d in work/recordings/*/; do
    scene=$(basename "$d")
    [ $# -gt 0 ] && [[ " $* " != *" $scene "* ]] && continue
    for cap in "$d"*.cap; do
        [ -e "$cap" ] || continue
        n=$((n + 1))
        r=$(build/fncheck --user work/testuser --capture "$cap" --ab 2>&1 | grep -v '^ *[0-9.]* \[' )
        line=$(echo "$r" | grep -E '^[0-9a-f]{8}: ' | tail -1)
        printf '%-8s %-22s %s\n' "$scene" "$(basename "$cap" .cap)" "${line#*: }"
        case "$line" in *": same "*) ;; *) fail=1; echo "$r" | grep -E '^  ' | head -8 ;; esac
    done
done
[ $n -eq 0 ] && { echo "no recordings: run tests/record.sh"; exit 1; }
exit $fail
