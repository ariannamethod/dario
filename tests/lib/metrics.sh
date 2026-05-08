#!/bin/bash
# metrics.sh — jq one-liners for extracting / aggregating test metrics.
#
# Each function reads JSONL on stdin (one /api/chat response per line)
# and writes a small report to stdout. Used by test_*.sh in the
# "summary" stage after the run.

# print min/mean/max of a numeric jq path, e.g. metric_stats '.resonance' < f.jsonl
metric_stats() {
    local path="$1"
    jq -r --arg p "$path" '
        ($p[1:]) as $k | .[$k]
    ' 2>/dev/null | awk '
        BEGIN { n=0; mn=1e30; mx=-1e30; s=0 }
        /^[+-]?[0-9.eE]+$/ {
            v=$1+0; n++; s+=v;
            if (v<mn) mn=v
            if (v>mx) mx=v
        }
        END {
            if (n==0) print "n=0"
            else printf("n=%d min=%.4f mean=%.4f max=%.4f\n", n, mn, s/n, mx)
        }'
}

# count occurrences of dominant_name (top term across the run)
dominant_histogram() {
    jq -r '.dominant_name' | sort | uniq -c | sort -rn
}

# value of one metric across all turns, prepended with turn number
metric_series() {
    local path="$1"
    jq -r --arg p "$path" '
        ($p[1:]) as $k | (input_line_number|tostring) + "\t" + (.[$k]|tostring)
    '
}
