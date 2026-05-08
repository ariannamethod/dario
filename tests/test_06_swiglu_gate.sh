#!/bin/bash
# test_06_swiglu_gate.sh
#
# WHAT IT TESTS
#   The SwiGLU visual gate (swiglu_gate(x, gate) = x * σ(gate)) used in
#   dario_compute to enrich H and F with visual co-occurrence and
#   visual prophecy: H_v[i] = swiglu_gate(H[i], vis_cooc[i]), and
#   F_v[i] similarly. Because vis_cooc is the visual-embedding-based
#   co-occurrence (D.vis_context · vis_embed[i]), this test sweeps the
#   field's resonance level by warming dario with N turns of poetry
#   first, then asks the same question and watches term_energy[H]
#   and term_energy[F].
#
# DESIGN
#   We can't directly inject a resonance — it is computed from cooc
#   density, dissonance, and debt. Instead we run six fresh dario
#   instances, each warmed with a different number of priming turns
#   (0, 5, 15, 40, 80, 150), then send identical "the organism
#   remembers" and capture term_energy + chambers + the metrics that
#   feed swiglu (resonance, entropy).
#
# METRICS PER PROBE
#   term_energy[H] (index 1), term_energy[F] (index 2),
#   resonance, entropy, emergence, dest_magnitude, vis_magnitude,
#   words generated.
#
# OUTPUT
#   tests/results/test_06_swiglu_gate.jsonl
#
# EXPECTED BEHAVIOR
#   At low priming (resonance ~ 0.3-0.4) H and F should be modest.
#   At higher resonance (cooc density saturates), the SwiGLU gate
#   amplifies H_v and F_v whenever vis_cooc is positive, so term
#   energies should grow.

set -euo pipefail
cd "$(dirname "$0")/.."

source tests/lib/api_client.sh
source tests/lib/metrics.sh

OUT="tests/results/test_06_swiglu_gate.jsonl"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"

if [ ! -x ./dario ]; then
    make all >/tmp/dario_test06_build.log 2>&1
fi

# warm-up corpus: short poetic lines from dario_essay.txt
WARMUP_FILE=$(mktemp)
tr '\n' ' ' <docs/dario_essay.txt \
  | tr -s ' ' '\n' \
  | awk 'NR%6==0{l=l" "$0; print l; l=""; next} {l=l" "$0}' \
  | sed 's/^ //' \
  | awk 'NF>=4 && NF<=10' \
  >"$WARMUP_FILE"
TOTAL_LINES=$(wc -l <"$WARMUP_FILE")
echo "[test_06] warmup corpus: $TOTAL_LINES lines"

PROBE='the organism remembers'
LEVELS=(0 5 15 40 80 150)
PORT=3106

for level in "${LEVELS[@]}"; do
    PORT=$((PORT + 1))
    start_dario "$PORT"
    if [ "$level" -gt 0 ]; then
        # warm-up turns: pull the first $level lines from corpus
        head -"$level" "$WARMUP_FILE" | while IFS= read -r line; do
            chat "$PORT" "$line" >/dev/null
        done
    fi
    # final probe (the metric line): same input across all levels
    j=$(chat "$PORT" "$PROBE")
    echo "$j" | jq -c \
        --argjson level "$level" \
        --arg probe "$PROBE" '{
            level: $level,
            probe: $probe,
            term_B:  .term_energy[0],
            term_H:  .term_energy[1],
            term_F:  .term_energy[2],
            term_A:  .term_energy[3],
            term_V:  .term_energy[4],
            term_S:  .term_energy[5],
            term_T:  .term_energy[6],
            resonance, entropy, emergence,
            dest_magnitude, vis_magnitude,
            cooc, vocab, step,
            words, dominant_name
        }' >>"$OUT"
    stop_dario
done
rm -f "$WARMUP_FILE"

echo "=== test_06 summary ==="
echo "out: $OUT"
echo "level | resonance | entropy | term_H | term_F | dom"
jq -r '
    [(.level|tostring), (.resonance|tostring), (.entropy|tostring),
     (.term_H|tostring), (.term_F|tostring), .dominant_name] | @tsv
' <"$OUT" | column -t -s "$(printf '\t')"
