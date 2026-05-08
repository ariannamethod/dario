#!/bin/bash
# test_02_dissonance_ramp.sh
#
# WHAT IT TESTS
#   compute_dissonance() and the trauma feedback loop in dario.c.
#   Six phases push progressively more out-of-vocab gibberish through
#   the field. After dissonance > 0.7, trauma_level should accumulate;
#   FEAR chamber should rise; tau (temperature) should increase
#   (organism gets noisier under stress).
#
# METRICS PER TURN
#   trauma, chamber.fear, dominant_name, tau, dissonance, vel
#
# OUTPUT
#   tests/results/test_02_dissonance_ramp.jsonl
#
# EXPECTED BEHAVIOR
#   Phase 0%: dissonance ~ 0.0, trauma flat, FEAR low.
#   Phase 60-100%: dissonance > 0.7, trauma climbing toward 1.0,
#     FEAR rising, dominant might shift to T:trauma occasionally.

set -euo pipefail
cd "$(dirname "$0")/.."

source tests/lib/api_client.sh
source tests/lib/metrics.sh

PORT=3102
TURNS_PER_PHASE=10
OUT="tests/results/test_02_dissonance_ramp.jsonl"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"

# real words drawn from dario's seed vocabulary (FIELD_WORDS in dario.c)
REAL=("resonance" "field" "destiny" "prophecy" "decay" "drift" "emerge" "coherence")
# pure gibberish — high entropy, definitely OOV
GIB=("zzxqpwkl" "fvbnmrrx" "qjqkpwxz" "vmnzbcxk" "tqyrlpxn" "zhxwqplv" "kxjvmnpq" "wzlpqxvn")

mk_input() {
    local pct="$1"
    local n="${2:-3}"  # words per input
    local out=""
    for _ in $(seq 1 "$n"); do
        if [ "$((RANDOM % 100))" -lt "$pct" ]; then
            out="$out ${GIB[$((RANDOM % ${#GIB[@]}))]}"
        else
            out="$out ${REAL[$((RANDOM % ${#REAL[@]}))]}"
        fi
    done
    echo "${out# }"
}

if [ ! -x ./dario ]; then
    make all >/tmp/dario_test02_build.log 2>&1
fi

start_dario "$PORT"
echo "[test_02] dario up on $PORT, ramping dissonance 0%→100%"

turn=0
for pct in 0 20 40 60 80 100; do
    for _ in $(seq 1 "$TURNS_PER_PHASE"); do
        turn=$((turn + 1))
        inp=$(mk_input "$pct" 4)
        j=$(chat "$PORT" "$inp")
        echo "$j" | jq -c --argjson turn "$turn" --argjson pct "$pct" --arg input "$inp" '{
            turn: $turn,
            phase_pct: $pct,
            input: $input,
            trauma, dissonance, tau,
            fear: .chambers.fear,
            love: .chambers.love,
            rage: .chambers.rage,
            void: .chambers.void,
            flow: .chambers.flow,
            complex: .chambers.complex,
            dominant_name, velocity,
            term_energy
        }' >>"$OUT"
    done
done

stop_dario

echo "=== test_02 summary ==="
echo "turns total: $turn  out: $OUT"
for pct in 0 20 40 60 80 100; do
    line=$(jq -s --argjson pct "$pct" '
        map(select(.phase_pct == $pct)) | {
            n: length,
            diss_mean: (map(.dissonance) | add / length),
            trauma_mean: (map(.trauma) | add / length),
            fear_mean: (map(.fear) | add / length),
            tau_mean: (map(.tau) | add / length)
        }' "$OUT")
    printf "  phase %3d%%  %s\n" "$pct" "$(echo "$line" | jq -c .)"
done
echo "dominant histogram:"
dominant_histogram <"$OUT"
