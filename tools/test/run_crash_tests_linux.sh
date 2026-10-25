#!/usr/bin/env bash
# Crash-handler end-to-end test runner for the CoD2x Linux dedicated server.
#
# For each selected mode it launches cod2_lnxded with COD2X_EXCEPTION_TEST=<mode>, waits for it to crash and
# exit, then checks the newest crash report (cod2x_crash.*.pending.json) for the markers that mode should
# produce. In test mode the server does NOT send the pending queue, so nothing reaches production.
#
# Requires a server built with the self test compiled in:
#     make build_linux_test DEBUG=0
# and a run directory containing cod2_lnxded, libCoD2x.so and the game data (main/). Run it there, in an
# environment that can execute the 32-bit server (e.g. the Docker image, or WSL Ubuntu 18.04 with
# libc6-i386 + libstdc++5:i386). The modes are documented in src/linux/exception.cpp.
#
# Usage:
#   GAME_DIR=/home/cod2 ./run_crash_tests_linux.sh            # all modes
#   GAME_DIR=/home/cod2 MODES="1 3 4" ./run_crash_tests_linux.sh
set -u

GAME_DIR="${GAME_DIR:-$(pwd)}"
MODES="${MODES:-1 2 3 4 5 6}"
TIMEOUT="${TIMEOUT:-25}"
NET_PORT="${NET_PORT:-28970}"
SERVER_BIN="${SERVER_BIN:-cod2_lnxded}"
LIB="${LIB:-libCoD2x.so}"

bin="$GAME_DIR/$SERVER_BIN"
lib="$GAME_DIR/$LIB"

if [[ ! -x "$bin" ]]; then echo "Server not found/executable: $bin"; echo "Build it: make build_linux_test DEBUG=0"; exit 2; fi
if [[ ! -f "$lib" ]]; then echo "Library not found: $lib"; exit 2; fi

# Per-mode: the signal the report must name, plus a note. Modes 5/6 also produce SIGSEGV.
declare -A SIG=( [1]=SIGSEGV [2]=SIGABRT [3]=SIGFPE [4]=SIGILL [5]=SIGSEGV [6]=SIGSEGV )
declare -A NAME=( [1]="null write" [2]="abort()" [3]="divide by zero" [4]="illegal instruction" [5]="stack overflow (altstack)" [6]="crash on second thread" )

echo "Server:  $bin"
echo "Modes:   $MODES"
echo

cleanup() { find "$GAME_DIR" -maxdepth 1 \( -name 'cod2x_crash.*' -o -name 'cod2x_crash_test_init.log' -o -name 'crashtest_server.log' \) -delete 2>/dev/null; }

pass=0; fail=0
for mode in $MODES; do
  echo "=== Mode $mode: ${NAME[$mode]:-unknown} ==="
  cleanup
  start=$(date +%s)

  ( cd "$GAME_DIR" && COD2X_EXCEPTION_TEST="$mode" LD_PRELOAD="$lib" \
      ./"$SERVER_BIN" +set dedicated 1 +set net_port "$NET_PORT" +set sv_master0 "" +set developer 1 \
      > "$GAME_DIR/crashtest_server.log" 2>&1 ) &
  pid=$!

  # Wait up to TIMEOUT for the server to crash and exit.
  waited=0
  while kill -0 "$pid" 2>/dev/null && (( waited < TIMEOUT )); do sleep 1; waited=$((waited+1)); done
  if kill -0 "$pid" 2>/dev/null; then
    echo "  server did not exit within ${TIMEOUT}s, killing it"
    kill -9 "$pid" 2>/dev/null
  fi
  wait "$pid" 2>/dev/null
  sleep 1

  report=$(find "$GAME_DIR" -maxdepth 1 -name 'cod2x_crash.*.pending.json' -newermt "@$((start-1))" 2>/dev/null | sort | tail -n1)
  marker="$GAME_DIR/cod2x_crash_test_init.log"

  ok=1; notes=()
  if [[ ! -f "$report" ]]; then
    ok=0; notes+=("no crash report produced")
    if [[ -f "$marker" ]]; then notes+=("marker: $(tr -d '\n' < "$marker")"); else notes+=("no init marker (server didn't reach exception_init / not a test build)"); fi
  else
    want="${SIG[$mode]}"
    grep -q "\"$want\"\|$want " "$report" || { ok=0; notes+=("missing signal $want"); }
    grep -q '"type":"linux-server"' "$report" || { ok=0; notes+=("missing type linux-server"); }
    grep -q "mode=$mode" "$marker" 2>/dev/null || notes+=("warn: init marker mode mismatch")
    notes+=("report: $(basename "$report")")

    # Delivery: a normal (non-test) restart runs crash_resendPending(), which POSTs the queued report and
    # deletes it on a 2xx. The backend acks test reports (crashInfo.test>0) without storing them.
    pending="$report"
    ( cd "$GAME_DIR" && LD_PRELOAD="$lib" ./"$SERVER_BIN" +set dedicated 1 +set net_port "$NET_PORT" \
        > "$GAME_DIR/crashtest_server.log" 2>&1 ) &
    dpid=$!
    dwaited=0
    while [[ -f "$pending" ]] && (( dwaited < 30 )); do sleep 1; dwaited=$((dwaited+1)); done
    kill -9 "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null
    if [[ -f "$pending" ]]; then
      ok=0; notes+=("telemetry NOT delivered (pending.json still present after resend)")
    else
      notes+=("telemetry: delivered")
    fi
  fi

  if (( ok )); then echo "  PASS  ${notes[*]}"; pass=$((pass+1)); else echo "  FAIL  ${notes[*]}"; fail=$((fail+1)); fi
  cleanup
  echo
done

echo "===== Summary ====="
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
