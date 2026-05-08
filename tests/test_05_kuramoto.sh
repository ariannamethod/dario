#!/bin/bash
# test_05_kuramoto.sh
#
# WHAT IT TESTS
#   chamber_update() in dario.c — six emotional chambers driven by
#   field metrics (dissonance, resonance, trauma, entropy, emergence)
#   and Kuramoto-coupled into the four modulators (α_mod, β_mod,
#   γ_mod, τ_mod).
#
# DESIGN
#   We feed three thematic clusters in 3-turn rotations for 60 turns
#   (20 cycles): LOVE-flavored prompts ("beautiful resonance love
#   warmth"), RAGE-flavored ("destroy corrupt broken shattered"),
#   VOID-flavored ("nothing empty silence void"). Note: chambers are
#   driven by the *metrics*, not by direct keyword dispatch — so the
#   real question is whether semantically distinct inputs reliably
#   shift the metrics enough to register in the chambers.
#
# METRICS PER TURN
#   chambers (6), alpha_mod, beta_mod, gamma_mod, tau_mod,
#   plus the cluster label so we can group later.
#
# OUTPUT
#   tests/results/test_05_kuramoto.jsonl
#
# EXPECTED BEHAVIOR
#   Chambers diverge between clusters. LOVE cluster pushes
#   chambers.love and alpha_mod up; RAGE cluster pushes
#   trauma+rage; VOID cluster pushes void+gamma_mod. If chambers
#   stay close to {0.001, …} regardless of input, that is itself
#   a finding — it would mean the emotional layer is decoupled
#   from input semantics under the current thresholds.

set -euo pipefail
cd "$(dirname "$0")/.."

source tests/lib/api_client.sh
source tests/lib/metrics.sh

PORT=3105
TURNS=60
OUT="tests/results/test_05_kuramoto.jsonl"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"

LOVE_INPUTS=(
    "beautiful resonance love warmth"
    "harmony radiant joy presence"
    "embrace tenderness blossom flow"
)
RAGE_INPUTS=(
    "destroy corrupt broken shattered"
    "violent rupture wound scream"
    "fracture splinter rot decay"
)
VOID_INPUTS=(
    "nothing empty silence void"
    "absence stillness vacant hollow"
    "dissolve fade vanish cease"
)

if [ ! -x ./dario ]; then
    make all >/tmp/dario_test05_build.log 2>&1
fi

start_dario "$PORT"
echo "[test_05] dario up on $PORT, $TURNS turns alternating LOVE/RAGE/VOID"

for i in $(seq 1 "$TURNS"); do
    cycle=$(( (i - 1) / 3 ))
    slot=$(( (i - 1) % 3 ))
    if [ "$slot" -eq 0 ]; then
        cluster="LOVE"
        idx=$(( cycle % ${#LOVE_INPUTS[@]} ))
        inp="${LOVE_INPUTS[$idx]}"
    elif [ "$slot" -eq 1 ]; then
        cluster="RAGE"
        idx=$(( cycle % ${#RAGE_INPUTS[@]} ))
        inp="${RAGE_INPUTS[$idx]}"
    else
        cluster="VOID"
        idx=$(( cycle % ${#VOID_INPUTS[@]} ))
        inp="${VOID_INPUTS[$idx]}"
    fi
    j=$(chat "$PORT" "$inp")
    echo "$j" | jq -c \
        --argjson turn "$i" \
        --arg cluster "$cluster" \
        --arg input "$inp" '{
            turn: $turn,
            cluster: $cluster,
            input: $input,
            fear: .chambers.fear,
            love: .chambers.love,
            rage: .chambers.rage,
            void: .chambers.void,
            flow: .chambers.flow,
            complex: .chambers.complex,
            alpha_mod, beta_mod, gamma_mod, tau_mod,
            resonance, dissonance, trauma, emergence,
            dominant_name
        }' >>"$OUT"
done

stop_dario

echo "=== test_05 summary ==="
echo "turns: $TURNS  out: $OUT"
for c in LOVE RAGE VOID; do
    line=$(jq -s --arg c "$c" '
        map(select(.cluster == $c)) | {
            n: length,
            love_mean: (map(.love)  | add / length),
            rage_mean: (map(.rage)  | add / length),
            void_mean: (map(.void)  | add / length),
            fear_mean: (map(.fear)  | add / length),
            flow_mean: (map(.flow)  | add / length),
            alpha_mod_mean: (map(.alpha_mod) | add / length),
            tau_mod_mean:   (map(.tau_mod)   | add / length)
        }' "$OUT")
    printf "  %-4s %s\n" "$c" "$(echo "$line" | jq -c .)"
done
echo "dominant by cluster:"
jq -r '.cluster + " " + .dominant_name' <"$OUT" | sort | uniq -c | sort -rn
