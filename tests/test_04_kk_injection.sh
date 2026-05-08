#!/bin/bash
# test_04_kk_injection.sh
#
# WHAT IT TESTS
#   The Knowledge Kernel modulation path (kk_modulate_field) inside
#   process_input. We pre-ingest the docs/ directory via the new
#   --ingest CLI flag, then send 20 turns:
#     - 10 topically related to docs (e.g. references to Bach,
#       mycorrhiza, bioluminescence, Polynesian nav, Byzantine icons,
#       Russian lit, dario_essay) — KK retrieval should hit and inject
#       prophecy + destiny nudges.
#     - 10 unrelated (random short field-words from dario seed vocab).
#
# METRICS PER TURN
#   prophecy_count, fulfillment_rate, dest_magnitude, emergence,
#   dominant_name (for shift detection)
#
# OUTPUT
#   tests/results/test_04_kk_injection.jsonl
#
# EXPECTED BEHAVIOR
#   On topical inputs, prophecy_count should rise (KK adds prophecies),
#   dest_magnitude shifts as KK nudges destiny, emergence may climb.
#   On unrelated inputs, KK lookup returns weak/empty hits — prophecy
#   adds slow down, dest_magnitude drifts on its own.

set -euo pipefail
cd "$(dirname "$0")/.."

source tests/lib/api_client.sh
source tests/lib/metrics.sh

PORT=3104
OUT="tests/results/test_04_kk_injection.jsonl"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"

if [ ! -x ./dario ]; then
    make all >/tmp/dario_test04_build.log 2>&1
fi

# pre-ingest docs/ via the new --ingest flag, then start web mode
DARIO_TEST_BIN_NEW=$(mktemp -d)/dario
cp ./dario "$DARIO_TEST_BIN_NEW"
chmod +x "$DARIO_TEST_BIN_NEW"

# we need --ingest BEFORE --web; api_client only knows how to launch with
# --web alone, so do it manually here.
rm -f dario_memory.db
"$DARIO_TEST_BIN_NEW" --ingest docs/ --web "$PORT" >/tmp/dario_test04.log 2>&1 &
DARIO_PID=$!
DARIO_LOG=/tmp/dario_test04.log
echo "[test_04] dario+KK starting on $PORT (PID=$DARIO_PID)"

if ! wait_ready "$PORT" 30; then
    echo "FATAL: dario --ingest --web never came up" >&2
    tail -50 "$DARIO_LOG" >&2
    exit 1
fi
INGESTED=$(grep "ingest" "$DARIO_LOG" | head -1 || true)
echo "[test_04] ingest line: $INGESTED"

# topical prompts targeting each docs/*.txt
TOPICAL=(
    "Bach counterpoint resonance harmony fugue"
    "mycorrhizal network roots fungal exchange"
    "bioluminescent jellyfish phosphorescent ocean"
    "Polynesian navigation stars wind ocean current"
    "Byzantine iconography gold mosaic saint"
    "Dickens Russian Tolstoy Dostoevsky novel"
    "dario equation resonance organism formula"
    "fungal hyphae carbon transfer forest"
    "polyphony voice independence equal weight"
    "icon prayer presence transcendent gold"
)
# unrelated prompts: pure dario seed vocabulary
UNRELATED=(
    "destiny prophecy decay drift"
    "field collapse threshold gradient"
    "phase emerge coherence dissonance"
    "harmonic frequency amplitude wave"
    "noise filter envelope transient"
    "saturation hysteresis attractor potential"
    "chain bigram token sequence"
    "vector embedding magnitude direction"
    "softmax temperature scaling sample"
    "trauma fear love rage void flow complex"
)

turn=0
record_turn() {
    local kind="$1"
    local inp="$2"
    turn=$((turn + 1))
    local j
    j=$(chat "$PORT" "$inp")
    echo "$j" | jq -c \
        --argjson turn "$turn" \
        --arg kind "$kind" \
        --arg input "$inp" '{
            turn: $turn,
            kind: $kind,
            input: $input,
            prophecy_count, prophecy_fulfilled, fulfillment_rate,
            dest_magnitude, vis_magnitude,
            emergence, resonance, debt, trauma,
            dominant_name, dissonance,
            term_energy
        }' >>"$OUT"
}

for inp in "${TOPICAL[@]}";    do record_turn "topical"   "$inp"; done
for inp in "${UNRELATED[@]}";  do record_turn "unrelated" "$inp"; done

stop_dario

echo "=== test_04 summary ==="
echo "turns: $turn  out: $OUT"
for k in topical unrelated; do
    line=$(jq -s --arg k "$k" '
        map(select(.kind == $k)) | {
            n: length,
            proph_mean: (map(.prophecy_count) | add / length),
            fulfil_mean: (map(.fulfillment_rate) | add / length),
            dest_mean: (map(.dest_magnitude) | add / length),
            emerg_mean: (map(.emergence) | add / length)
        }' "$OUT")
    printf "  %-9s %s\n" "$k" "$(echo "$line" | jq -c .)"
done
echo "dominant histogram (topical vs unrelated):"
jq -r '.kind + " " + .dominant_name' <"$OUT" | sort | uniq -c | sort -rn
