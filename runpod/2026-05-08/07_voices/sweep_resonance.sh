#!/usr/bin/env bash
set +e
mkdir -p 07_voices/transcripts_resonance
SCORES=07_voices/scores_resonance.tsv
echo -e "voice\tprompt\ttemp\ttop_p\tbytes\ttok_per_s\ttoks" > "$SCORES"
COUNT=0
RESONANCE=/workspace/resonance.aml/resonance
cd /workspace/resonance.aml
for pid in 1 2 3; do
  case "$pid" in
    1) PROMPT="Q: What is the RRPRAM mechanism inside Janus attention?\nA:" ;;
    2) PROMPT="Q: Does memory create identity, or does identity create memory?\nA:" ;;
    3) PROMPT="Q: Tell me what you remember most clearly from before.\nA:" ;;
  esac
  for temp in 0.3 0.5 0.7 0.8 0.9 1.0; do
    for tp in 0.9 1.0; do
      OUT="/workspace/dario/07_voices/transcripts_resonance/resonance_t${temp}_p${tp}_p${pid}.txt"
      timeout 30 "$RESONANCE" -p "$PROMPT" -n 100 -t "$temp" --top-p "$tp" > "$OUT" 2>&1 || echo "TIMEOUT" >> "$OUT"
      BYTES=$(wc -c < "$OUT")
      TOKS=$(grep -oE "[0-9]+ tokens" "$OUT" | head -1 | grep -oE "^[0-9]+")
      TPS=$(grep -oE "[0-9.]+ tok/s" "$OUT" | head -1 | grep -oE "^[0-9.]+")
      echo -e "resonance-yent\t${pid}\t${temp}\t${tp}\t${BYTES}\t${TPS:-NA}\t${TOKS:-NA}" >> "/workspace/dario/$SCORES"
      COUNT=$((COUNT+1))
    done
  done
done
echo "[sweep_resonance] $COUNT cells generated"
