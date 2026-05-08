#!/bin/bash
# api_client.sh — curl wrapper + dario lifecycle helpers for tests/.
#
# Sourced by every test_*.sh. Provides:
#   start_dario  PORT [BIN] [LOGFILE]   start binary in background, store PID, wait until ready
#   stop_dario                          SIGTERM the bg dario, wait for exit
#   chat         PORT TEXT              POST /api/chat, write JSON to stdout
#   chat_q       PORT TEXT              chat then jq -c '.' (compact one-line JSON)
#   wait_ready   PORT [TIMEOUT_S]       poll /api/chat until 200, hard fail after timeout
#
# Each test runs on its own port so harness invocations cannot collide.
# The dario process and its sqlite memory file are torn down via stop_dario,
# which traps EXIT so even ^C / errors get cleaned.

set -u

DARIO_TEST_BIN="${DARIO_TEST_BIN:-./dario}"
DARIO_TEST_HTML="${DARIO_TEST_HTML:-dario.html}"
DARIO_TEST_TIMEOUT="${DARIO_TEST_TIMEOUT:-25}"

# global: PID and log path of the active background dario, if any
DARIO_PID=""
DARIO_LOG=""

start_dario() {
    local port="$1"
    local bin="${2:-$DARIO_TEST_BIN}"
    local log="${3:-/tmp/dario_test_${port}.log}"

    if [ ! -x "$bin" ]; then
        echo "FATAL: dario binary not executable: $bin" >&2
        return 1
    fi

    # remove any leftover sqlite from a previous run so each test starts fresh
    rm -f dario_memory.db
    "$bin" --web "$port" >"$log" 2>&1 &
    DARIO_PID=$!
    DARIO_LOG="$log"

    # wait until the server actually answers
    if ! wait_ready "$port" "$DARIO_TEST_TIMEOUT"; then
        echo "FATAL: dario on port $port never came up" >&2
        echo "--- tail of $log ---" >&2
        tail -20 "$log" >&2
        stop_dario
        return 1
    fi
    return 0
}

stop_dario() {
    # Returns 0 even when wait reports a signal exit so callers under `set -e`
    # are not killed by a clean shutdown.
    if [ -n "$DARIO_PID" ]; then
        kill -TERM "$DARIO_PID" 2>/dev/null || true
        # 1s grace, then SIGKILL
        for _ in 1 2 3 4 5; do
            if ! kill -0 "$DARIO_PID" 2>/dev/null; then break; fi
            sleep 0.2
        done
        kill -9 "$DARIO_PID" 2>/dev/null || true
        wait "$DARIO_PID" 2>/dev/null || true
        DARIO_PID=""
    fi
    return 0
}

# poll /api/chat until it returns 200 (or timeout)
wait_ready() {
    local port="$1"
    local timeout="${2:-25}"
    local t0
    t0=$(date +%s)
    while :; do
        local code
        code=$(curl -s -o /dev/null -m 2 -w "%{http_code}" \
                  -X POST "http://localhost:${port}/api/chat" \
                  -H "Content-Type: application/json" \
                  -d '{"text":"ping"}' 2>/dev/null || echo "000")
        if [ "$code" = "200" ]; then return 0; fi
        local now
        now=$(date +%s)
        if [ $((now - t0)) -ge "$timeout" ]; then
            return 1
        fi
        sleep 0.2
    done
}

# POST /api/chat and emit raw JSON
chat() {
    local port="$1"; shift
    local text="$*"
    # escape backslashes and double quotes for JSON body
    local esc
    esc=$(printf '%s' "$text" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g')
    curl -s -m 10 -X POST "http://localhost:${port}/api/chat" \
        -H "Content-Type: application/json" \
        --data "{\"text\":\"${esc}\"}"
}

# POST /api/chat and emit compact JSON via jq (one line)
chat_q() {
    chat "$@" | jq -c '.'
}

# trap: ensure dario is killed on normal exit, ^C, and SIGTERM.
# Per Codex review, splitting these so signals still terminate with a
# distinguishable status (130 for SIGINT, 143 for SIGTERM) instead of
# being silently swallowed by a return-0 cleanup.
trap 'stop_dario' EXIT
trap 'stop_dario; exit 130' INT
trap 'stop_dario; exit 143' TERM
