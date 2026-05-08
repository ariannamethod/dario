#!/bin/bash
# test_07_prophecy_debt.sh
#
# WHAT IT TESTS
#   The prophecy-debt accumulator in generate_words(): every
#   generation step where the chosen token's logit is below the
#   maximum adds (max - chosen) / (max - chosen + 1) to D.debt.
#   Debt feeds back into the F (prophecy) term via prophecy
#   strengths and into resonance via the (1 - clamp(debt*0.1)) term.
#
# DESIGN
#   200 turns with semi-repetitive "poetry" inputs (we lift two-line
#   stanzas from dario_essay.txt). Repetitive structure should drive
#   strong bigram pull → mostly STOP velocity → low τ → distribution
#   sharpens → debt grows steadily as second-best tokens get pushed
#   aside. Watch when MAX_COOC=65536 saturates.
#
# METRICS PER TURN
#   debt, prophecy_count, prophecy_fulfilled, fulfillment_rate,
#   resonance, cooc, vocab, step, dominant_name
#
# OUTPUT
#   tests/results/test_07_prophecy_debt.jsonl
#
# EXPECTED BEHAVIOR
#   debt grows roughly linearly with step. fulfillment_rate may stay
#   low because prophecies are aged out. cooc.n grows then plateaus.
#   We log MAX_COOC threshold crossings if/when they happen.

set -euo pipefail
cd "$(dirname "$0")/.."

source tests/lib/api_client.sh
source tests/lib/metrics.sh

PORT=3107
TURNS=200
OUT="tests/results/test_07_prophecy_debt.jsonl"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"

if [ ! -x ./dario ]; then
    make all >/tmp/dario_test07_build.log 2>&1
fi

# poetic feed: short rhythmic lines from dario_essay
TMP_RAW=$(mktemp)
TMP_INP=$(mktemp)
tr '\n' ' ' <docs/dario_essay.txt \
  | tr -s ' ' '\n' \
  | awk 'NR%5==0{l=l" "$0; print l; l=""; next} {l=l" "$0}' \
  | sed 's/^ //' \
  | awk 'NF>=4 && NF<=8' \
  >>"$TMP_RAW"
head -"$TURNS" "$TMP_RAW" >"$TMP_INP"
N=$(wc -l <"$TMP_INP")
echo "[test_07] poetry pool: $N lines (need $TURNS — will recycle if short)"
rm -f "$TMP_RAW"

# load array
INPUTS=()
while IFS= read -r line; do
    INPUTS+=("$line")
done <"$TMP_INP"
rm -f "$TMP_INP"

start_dario "$PORT"
echo "[test_07] dario up on $PORT, $TURNS poetry turns"

last_cooc=0
saturation_step=-1
for i in $(seq 1 "$TURNS"); do
    inp="${INPUTS[$(( (i - 1) % ${#INPUTS[@]} ))]}"
    j=$(chat "$PORT" "$inp")
    cooc=$(echo "$j" | jq -r '.cooc')
    # detect saturation (MAX_COOC = 65536)
    if [ "$saturation_step" -lt 0 ] && [ "$cooc" -ge 65500 ]; then
        saturation_step="$i"
    fi
    last_cooc="$cooc"
    echo "$j" | jq -c \
        --argjson turn "$i" '{
            turn: $turn,
            debt, resonance, dissonance, trauma, momentum,
            prophecy_count, prophecy_fulfilled, fulfillment_rate,
            cooc, vocab, bigrams, step,
            dominant_name, velocity
        }' >>"$OUT"
done

stop_dario

echo "=== test_07 summary ==="
echo "turns: $TURNS  out: $OUT"
echo -n "debt:           "; metric_stats '.debt'             <"$OUT"
echo -n "prophecy_count: "; metric_stats '.prophecy_count'   <"$OUT"
echo -n "fulfillment:    "; metric_stats '.fulfillment_rate' <"$OUT"
echo -n "cooc:           "; metric_stats '.cooc'             <"$OUT"
echo -n "vocab:          "; metric_stats '.vocab'            <"$OUT"
echo "saturation_step: $saturation_step (last cooc: $last_cooc / 65536)"
echo "velocity histogram:"
jq -r '.velocity' <"$OUT" | sort | uniq -c | sort -rn
echo "dominant histogram:"
dominant_histogram <"$OUT"
