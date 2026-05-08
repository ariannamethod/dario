#!/usr/bin/env bash
set -e
mkdir -p 07_voices/transcripts
USE_CHAT="${USE_CHAT:-1}"
resolve_weights() {
  case "$1" in
    leo)            echo weights/janus_v4_sft_leo.bin ;;
    arianna)        echo weights/janus_v4_sft_arianna.bin ;;
    yent)           echo weights/janus_v4_sft_yent.bin ;;
    resonance-yent) echo weights/resonance_200m_lora_yent.bin ;;
    leo24m)         echo weights/leo_janus_d12_f16.bin ;;
    *) echo ""; return 1 ;;
  esac
}
SCORES=07_voices/scores.tsv
echo -e "voice\tprompt\ttemp\ttopk\trp\tbytes\ttok_per_s\ttoks" > "$SCORES"
COUNT=0
for voice in leo arianna yent resonance-yent leo24m; do
  WEIGHTS=$(resolve_weights "$voice")
  case "$voice" in
    leo|arianna|yent) [ "$USE_CHAT" = "1" ] && CHAT_FLAG="--chat-tokens" || CHAT_FLAG="" ;;
    *)                CHAT_FLAG="" ;;
  esac
  for pid in 1 2 3; do
    case "$pid" in
      1) PROMPT="What is the RRPRAM mechanism inside Janus attention?" ;;
      2) PROMPT="Does memory create identity, or does identity create memory?" ;;
      3) PROMPT="Tell me what you remember most clearly from before." ;;
    esac
    for temp in 0.3 0.5 0.7 0.8 0.9 1.0; do
      for topk in 40 0; do
        for rp in 1.0 1.3 1.4; do
          OUT="07_voices/transcripts/${voice}_t${temp}_k${topk}_rp${rp}_p${pid}.txt"
          ./infer_v4 "$WEIGHTS" "Q: $PROMPT\nA:" 100 "$temp" 42 "$topk" --rep-penalty "$rp" $CHAT_FLAG > "$OUT" 2>&1 || true
          BYTES=$(wc -c < "$OUT")
          TOKS=$(grep -oE "[0-9]+ tokens" "$OUT" | head -1 | grep -oE "^[0-9]+")
          TPS=$(grep -oE "[0-9.]+ tok/s" "$OUT" | head -1 | grep -oE "^[0-9.]+")
          echo -e "${voice}\t${pid}\t${temp}\t${topk}\t${rp}\t${BYTES}\t${TPS:-NA}\t${TOKS:-NA}" >> "$SCORES"
          COUNT=$((COUNT+1))
        done
      done
    done
  done
done
echo "[sweep] done: $COUNT cells generated"
