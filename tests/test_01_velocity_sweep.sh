#!/bin/bash
# test_01_velocity_sweep.sh
#
# WHAT IT TESTS
#   The velocity automaton (auto_velocity / apply_velocity in dario.c).
#   When the same input is fed 50 times in a row, the field should:
#     - quickly stop seeing dissonance (input is in vocab),
#     - lift resonance toward 1.0,
#     - eventually transition WALK → STOP (debt drops, momentum drains),
#     - keep trauma_level at 0 (since dissonance < 0.7 throughout).
#
# METRICS PER TURN
#   term_energy[B,H,F,A,V,T], velocity, resonance, trauma_level, debt
#
# OUTPUT
#   tests/results/test_01_velocity_sweep.jsonl  (one JSON line per turn)
#
# EXPECTED BEHAVIOR
#   Run starts in WALK with high resonance. After ~20-30 turns the
#   velocity should settle (likely STOP/BREATHE) and term[A:destiny]
#   should dominate (the EMA destiny vector concentrates on the same
#   two-token pair). debt stays low. trauma == 0 throughout.

set -euo pipefail
cd "$(dirname "$0")/.."

# shellcheck source=lib/api_client.sh
source tests/lib/api_client.sh
# shellcheck source=lib/metrics.sh
source tests/lib/metrics.sh

PORT=3101
TURNS=50
INPUT="resonance field"
OUT="tests/results/test_01_velocity_sweep.jsonl"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"

if [ ! -x ./dario ]; then
    echo "[test_01] building dario (make all)..." >&2
    make all >/tmp/dario_test01_build.log 2>&1
fi

start_dario "$PORT"
echo "[test_01] dario up on $PORT, sending $TURNS × \"$INPUT\""

for i in $(seq 1 "$TURNS"); do
    j=$(chat "$PORT" "$INPUT")
    # extract just the relevant slice; drop noisy code/words to keep file small
    echo "$j" | jq -c --argjson turn "$i" '{
        turn: $turn,
        velocity, resonance, trauma, debt,
        dominant_name, dissonance, tau, vel_temp,
        term_energy
    }' >>"$OUT"
done

stop_dario

echo "=== test_01 summary ==="
echo "turns: $TURNS  out: $OUT"
echo -n "resonance:  "; metric_stats '.resonance'  <"$OUT"
echo -n "debt:       "; metric_stats '.debt'       <"$OUT"
echo -n "trauma:     "; metric_stats '.trauma'     <"$OUT"
echo -n "dissonance: "; metric_stats '.dissonance' <"$OUT"
echo "velocity histogram:"
jq -r '.velocity' <"$OUT" | sort | uniq -c | sort -rn
echo "dominant histogram:"
dominant_histogram <"$OUT"
