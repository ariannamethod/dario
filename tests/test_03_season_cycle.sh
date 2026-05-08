#!/bin/bash
# test_03_season_cycle.sh
#
# WHAT IT TESTS
#   The seasonal automaton in dario.c (season_step).
#   season_phase += 0.002 per turn → full cycle of 4 seasons in 500 turns
#   (well, the phase wraps every 500 turns; transitions add up to 2000 for
#   a complete spring→winter→spring loop, so we are sampling roughly 1
#   season's worth of drift in 500 turns; still plenty to see at least
#   one season transition).
#   Each season nudges a coefficient (β in spring, α in summer, autumn
#   boosts bigrams, winter pushes trauma).
#
# METRICS PER TURN
#   season_phase, season_name, alpha, beta, gamma, tau, dominant_term
#
# OUTPUT
#   tests/results/test_03_season_cycle.jsonl
#
# EXPECTED BEHAVIOR
#   Over 500 turns we should see β rise during spring and (after a
#   season switch) see α rise. dominant_term should drift as
#   coefficients drift.

set -euo pipefail
cd "$(dirname "$0")/.."

source tests/lib/api_client.sh
source tests/lib/metrics.sh

PORT=3103
TURNS=500
OUT="tests/results/test_03_season_cycle.jsonl"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"

# rotating input pool: pull short chunks from each docs/*.txt.
# We want diversity so the field doesn't lock onto one term.
# bash 3.2 has no `mapfile`, so we build the array with a tmp file + read loop.
DOCS_DIR="docs"
TMP_INPUTS=$(mktemp)
TMP_RAW=$(mktemp)
# `head | …` upstream of a `set -o pipefail` script trips SIGPIPE (141);
# write everything to a tmp file first, then truncate.
for f in "$DOCS_DIR"/*.txt; do
    tr '\n' ' ' <"$f" \
      | tr -s ' ' '\n' \
      | awk 'NR%6==0{l=l" "$0; print l; l=""; next} {l=l" "$0}' \
      | sed 's/^ //' \
      | awk 'NF>=3 && NF<=12 {print}' >>"$TMP_RAW"
done
head -800 "$TMP_RAW" >"$TMP_INPUTS"
rm -f "$TMP_RAW"

INPUTS=()
while IFS= read -r line; do
    INPUTS+=("$line")
done <"$TMP_INPUTS"
rm -f "$TMP_INPUTS"
N_INPUTS=${#INPUTS[@]}
if [ "$N_INPUTS" -lt 50 ]; then
    echo "FATAL: not enough input chunks ($N_INPUTS); check docs/" >&2
    exit 1
fi
echo "[test_03] input pool: $N_INPUTS chunks from docs/"

if [ ! -x ./dario ]; then
    make all >/tmp/dario_test03_build.log 2>&1
fi

start_dario "$PORT"
echo "[test_03] dario up on $PORT, running $TURNS turns"

for i in $(seq 1 "$TURNS"); do
    inp="${INPUTS[$(( (i - 1) % N_INPUTS ))]}"
    j=$(chat "$PORT" "$inp")
    echo "$j" | jq -c --argjson turn "$i" '{
        turn: $turn,
        season, dominant_name,
        alpha, beta, gamma,
        alpha_mod, beta_mod, gamma_mod,
        tau, tau_mod, vel_temp,
        velocity, dissonance, trauma, debt, resonance,
        term_energy
    }' >>"$OUT"
done

stop_dario

echo "=== test_03 summary ==="
echo "turns: $TURNS  out: $OUT"
echo "season histogram:"
jq -r '.season' <"$OUT" | sort | uniq -c | sort -rn
echo "dominant histogram:"
dominant_histogram <"$OUT"
echo "alpha range:"; metric_stats '.alpha' <"$OUT"
echo "beta range: "; metric_stats '.beta'  <"$OUT"
echo "gamma range:"; metric_stats '.gamma' <"$OUT"
echo "tau range:  "; metric_stats '.tau'   <"$OUT"
# per-season averages
echo "per-season coefficient means:"
for s in spring summer autumn winter; do
    line=$(jq -s --arg s "$s" '
        map(select(.season == $s)) | if length == 0 then null else {
            n: length,
            alpha: (map(.alpha) | add / length),
            beta:  (map(.beta)  | add / length),
            gamma: (map(.gamma) | add / length),
            tau:   (map(.tau)   | add / length)
        } end' "$OUT")
    printf "  %-7s %s\n" "$s" "$(echo "$line" | jq -c .)"
done
