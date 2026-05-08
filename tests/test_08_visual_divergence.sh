#!/bin/bash
# test_08_visual_divergence.sh
#
# WHAT IT TESTS
#   The δ·V (visual) term in dario_compute. Visual embeddings are
#   per-word perceptual hashes (g_vis_embeds, init from siphash) and
#   the visual context vis_context is an EMA of those vectors. The
#   semantic destiny g_destiny is an EMA of g_embeds (semantic
#   hashes). When the user pushes one cluster of inputs, both
#   destinies drift toward that cluster's centroid; if we then flip
#   to a *very different* cluster, visual destiny should adapt
#   faster than semantic destiny because vis_embeds are unrelated
#   noise vectors per word — there is no semantic cooc memory in
#   the visual track.
#
# DESIGN
#   30 turns:
#     - turns 1-15  : cluster A (e.g. "sunlight ocean salt wind")
#     - turns 16-30 : cluster B with maximally different words
#                     ("granite cobalt thorn mortar")
#   We capture term_V, term_A, vis_magnitude, dest_magnitude per
#   turn, plus the dominant term so we can see when V overtakes A.
#
# OUTPUT
#   tests/results/test_08_visual_divergence.jsonl
#
# EXPECTED BEHAVIOR
#   Phase A: term_V and term_A both grow as the EMA fills.
#   Phase B turn 16-20: vis_magnitude dips then re-rises (EMA shifts
#     fast: 0.1 weight on new); semantic destiny clings (90% inertia
#     in destiny EMA + cooc memory). term_V should pull ahead briefly,
#     then settle.

set -euo pipefail
cd "$(dirname "$0")/.."

source tests/lib/api_client.sh
source tests/lib/metrics.sh

PORT=3108
TURNS=30
SWITCH=15
OUT="tests/results/test_08_visual_divergence.jsonl"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"

CLUSTER_A=(
    "sunlight ocean salt wind"
    "wave shore tide foam"
    "bright surface horizon spray"
    "warm current open sky"
    "drift glide breath calm"
)
CLUSTER_B=(
    "granite cobalt thorn mortar"
    "iron rust forge coal"
    "blade scaffold hammer bolt"
    "lead steel cinder ash"
    "bunker concrete spike fence"
)

if [ ! -x ./dario ]; then
    make all >/tmp/dario_test08_build.log 2>&1
fi

start_dario "$PORT"
echo "[test_08] dario up on $PORT, switching cluster at turn $SWITCH"

for i in $(seq 1 "$TURNS"); do
    if [ "$i" -le "$SWITCH" ]; then
        cluster="A"
        idx=$(( (i - 1) % ${#CLUSTER_A[@]} ))
        inp="${CLUSTER_A[$idx]}"
    else
        cluster="B"
        idx=$(( (i - SWITCH - 1) % ${#CLUSTER_B[@]} ))
        inp="${CLUSTER_B[$idx]}"
    fi
    j=$(chat "$PORT" "$inp")
    echo "$j" | jq -c \
        --argjson turn "$i" \
        --arg cluster "$cluster" \
        --arg input "$inp" '{
            turn: $turn,
            cluster: $cluster,
            input: $input,
            term_V: .term_energy[4],
            term_A: .term_energy[3],
            term_H: .term_energy[1],
            term_F: .term_energy[2],
            vis_magnitude, dest_magnitude,
            resonance, dissonance, trauma,
            dominant_name, velocity
        }' >>"$OUT"
done

stop_dario

echo "=== test_08 summary ==="
echo "turns: $TURNS  switch: $SWITCH  out: $OUT"
for c in A B; do
    line=$(jq -s --arg c "$c" '
        map(select(.cluster == $c)) | {
            n: length,
            term_V_mean: (map(.term_V) | add / length),
            term_A_mean: (map(.term_A) | add / length),
            vis_mean:    (map(.vis_magnitude)  | add / length),
            dest_mean:   (map(.dest_magnitude) | add / length),
            res_mean:    (map(.resonance)      | add / length)
        }' "$OUT")
    printf "  %s %s\n" "$c" "$(echo "$line" | jq -c .)"
done
echo "near-switch trajectory (turns ${SWITCH}..$((SWITCH+5))):"
jq -r --argjson sw "$SWITCH" '
    select(.turn >= $sw and .turn <= ($sw + 5)) |
    [(.turn|tostring), .cluster, (.term_V|tostring), (.term_A|tostring),
     (.vis_magnitude|tostring), (.dest_magnitude|tostring), .dominant_name] | @tsv
' <"$OUT" | column -t -s "$(printf '\t')"
echo "dominant by cluster:"
jq -r '.cluster + " " + .dominant_name' <"$OUT" | sort | uniq -c | sort -rn
