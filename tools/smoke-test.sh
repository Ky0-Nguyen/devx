#!/usr/bin/env bash
# End-to-end CLI checks that the unit tests cannot reach: argument parsing,
# exit codes, and the session lifecycle on disk.
#
# Usage: tools/smoke-test.sh [path-to-mpi]
set -uo pipefail

MPI="${1:-build/bin/mpi}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ ! -x "$MPI" ]; then
  echo "error: $MPI not found; run: cmake --build build" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

check() { # check <description> <expected-exit> <command...>
  local desc="$1" want="$2"; shift 2
  "$@" >"$TMP/out" 2>"$TMP/err"
  local got=$?
  if [ "$got" = "$want" ]; then
    printf '  ok   %s\n' "$desc"
    pass=$((pass+1))
  else
    printf '  FAIL %s (expected exit %s, got %s)\n' "$desc" "$want" "$got"
    sed 's/^/       /' "$TMP/err" | head -3
    fail=$((fail+1))
  fi
}

check_contains() { # check_contains <description> <needle> <command...>
  local desc="$1" needle="$2"; shift 2
  # The output is captured to a file rather than piped into grep. Under
  # `pipefail`, `cmd | grep -q` reports failure whenever grep exits on its
  # first match early enough to hand the producer a SIGPIPE -- which made this
  # harness fail precisely when the needle appeared near the top of the output.
  "$@" >"$TMP/contains" 2>&1
  if grep -qF -- "$needle" "$TMP/contains"; then
    printf '  ok   %s\n' "$desc"
    pass=$((pass+1))
  else
    printf '  FAIL %s (output did not contain: %s)\n' "$desc" "$needle"
    fail=$((fail+1))
  fi
}

echo "== argument parsing =="
check "no command is a usage error" 2 "$MPI"
check "unknown command is a usage error" 2 "$MPI" not-a-command
check "--help succeeds" 0 "$MPI" --help
check "bad --platform is rejected" 2 "$MPI" devices --platform windows
check "bad --timeout-ms is rejected" 2 "$MPI" devices --timeout-ms 0
check "analyze with no argument is a usage error" 2 "$MPI" analyze
check "compare with one argument is a usage error" 2 "$MPI" compare a.json
check "bad --format is rejected" 2 "$MPI" analyze fixtures/traces/negative-healthy.mpi.json --format xml
check "malformed --threshold is rejected" 2 "$MPI" analyze fixtures/traces/negative-healthy.mpi.json --threshold nope
check "--suppress without a reason is refused" 2 "$MPI" analyze fixtures/traces/negative-healthy.mpi.json --suppress DET-01

# Every flag a subcommand reads with inv.flag() must consume a value. A flag
# missing from needs_value() would silently turn its value into a positional.
echo "== value-taking flag list agrees with the subcommands =="
declared="$(sed -n '/kWithValue\[\]/,/};/p' apps/cli/main.cpp \
  | grep -oE '"--[a-z0-9-]+"' | tr -d '"' | sed 's/^--//' | sort -u)"
used="$(grep -ohE 'inv\.flag\("[a-z0-9-]+"' apps/cli/*.cpp \
  | sed 's/inv\.flag("//; s/"//' | sort -u)"
missing="$(comm -13 <(echo "$declared") <(echo "$used"))"
if [ -z "$missing" ]; then
  printf '  ok   every inv.flag() name consumes a value\n'; pass=$((pass+1))
else
  printf '  FAIL these flags are read with a value but parsed as booleans: %s\n' "$(echo $missing)"
  fail=$((fail+1))
fi

# A flag the parser does not know must be refused, not ignored. `--duration 14`
# (the real flag is --duration-s) recorded the default 5 s and reported
# success, which hands back numbers for a window the caller never asked for.
check "an unknown flag is refused" 2 "$MPI" record --device d --app a --duration 14
check "a misspelled boolean flag is refused" 2 "$MPI" record --device d --app a --lives

# And every flag the subcommands actually read must be accepted, so the
# vocabulary cannot drift from the parser.
echo "== every flag the subcommands read is known to the parser =="
readers="$(grep -ohE 'inv\.(flag|has_flag)\("[a-z0-9-]+"' apps/cli/*.cpp \
  | sed 's/.*("//; s/"//' | sort -u)"
vocabulary="$(sed -n '/kWithValue\[\]/,/};/p;/kBoolean\[\]/,/};/p' apps/cli/main.cpp \
  | grep -oE '"--[a-z0-9-]+"' | tr -d '"' | sed 's/^--//' | sort -u)"
unknown="$(comm -13 <(echo "$vocabulary") <(echo "$readers"))"
if [ -z "$unknown" ]; then
  printf '  ok   every flag a subcommand reads is accepted by the parser\n'; pass=$((pass+1))
else
  printf '  FAIL these flags are read but would be refused: %s\n' "$(echo $unknown)"
  fail=$((fail+1))
fi

# Launch-and-record has to refuse a bad launch class rather than launching
# something the operator did not ask for (spec A21).
check "a bad --launch-class is refused" 2 "$MPI" record --device d --app a --launch --launch-class=tepid
check "--wait-for-app-s must be positive" 2 "$MPI" record --device d --app a --launch --wait-for-app-s 0

# The SDK's own end-to-end test, when node is available. It drives the real
# JavaScript client over real HTTP into the real ingest, which is the only way
# the sequence and backpressure behaviour is exercised as a pair.
if command -v node >/dev/null 2>&1; then
  echo "== app SDK end to end (node) =="
  if node sdk/react-native/test/e2e.mjs "$MPI" > "$TMP/sdk-e2e.log" 2>&1; then
    printf '  ok   the React Native SDK drives the real endpoint (%s checks)\n' \
      "$(grep -c '  ok ' "$TMP/sdk-e2e.log")"; pass=$((pass+1))
  else
    printf '  FAIL the SDK end-to-end test failed:\n'; sed 's/^/       /' "$TMP/sdk-e2e.log" | tail -20
    fail=$((fail+1))
  fi
  # The sample app's own instrumentation module, driven the way its screens
  # drive it. `App.js` needs React Native and is not built here, but the module
  # it imports is plain JavaScript -- so the part a developer copies is the
  # part that gets tested.
  if node samples/react-native/verify.mjs "$MPI" > "$TMP/sample.log" 2>&1; then
    printf '  ok   the sample app'"'"'s instrumentation drives the real endpoint (%s checks)\n' \
      "$(grep -c '  ok ' "$TMP/sample.log")"; pass=$((pass+1))
  else
    printf '  FAIL the sample app verification failed:\n'; sed 's/^/       /' "$TMP/sample.log" | tail -20
    fail=$((fail+1))
  fi
else
  printf '  skip node is not installed, so neither the SDK end-to-end test nor the sample verification ran\n'
fi

echo "== analysis exit codes =="
check "healthy fixture analyzes ok" 0 "$MPI" analyze fixtures/traces/negative-healthy.mpi.json --format json --out "$TMP/a.json"
check "positive fixture analyzes ok" 0 "$MPI" analyze fixtures/traces/positive-frames-js-cpu.mpi.json --format json --out "$TMP/b.json"
check "malformed trace is a collection error" 5 "$MPI" analyze fixtures/traces/malformed-truncated.json
check "empty file is a collection error" 5 "$MPI" analyze fixtures/traces/malformed-empty.json
check "binary garbage is a collection error" 5 "$MPI" analyze fixtures/traces/malformed-not-json.bin
check "missing file is a collection error" 5 "$MPI" analyze "$TMP/does-not-exist.json"
check "a tiny --max-input-mib refuses a large trace" 5 "$MPI" analyze fixtures/traces/positive-frames-js-cpu.mpi.json --max-input-mib 1

echo "== honesty invariants in the rendered report =="
check_contains "synthetic input is announced" "SYNTHETIC DATA" \
  "$MPI" analyze fixtures/traces/positive-frames-js-cpu.mpi.json
check_contains "a skip is not an all-clear" "found **nothing because it did not run**" \
  "$MPI" analyze fixtures/traces/negative-healthy.mpi.json
check_contains "detectors ran on the healthy fixture" "ran_found_nothing" \
  "$MPI" analyze fixtures/traces/negative-healthy.mpi.json
check_contains "a gap is not measured zero" "not measured zero activity" \
  "$MPI" analyze fixtures/traces/incomplete-evidence.mpi.json
check_contains "a partial capture is announced" "PARTIAL CAPTURE" \
  "$MPI" analyze fixtures/traces/incomplete-evidence.mpi.json
check_contains "a diagnostic session cannot certify release" "cannot certify release performance" \
  "$MPI" analyze fixtures/traces/positive-frames-js-cpu.mpi.json
check_contains "overhead must not be subtracted" "must not be subtracted" \
  "$MPI" analyze fixtures/traces/positive-frames-js-cpu.mpi.json

echo "== third-party trace formats =="
check "hermes profile analyzes" 0 "$MPI" analyze fixtures/traces/hermes-profile.json --format json --out "$TMP/h.json"
check "chrome trace analyzes" 0 "$MPI" analyze fixtures/traces/chrome-trace-event.json --format json --out "$TMP/c.json"
check_contains "hermes samples are not turned into task durations" "no task boundaries" \
  "$MPI" analyze fixtures/traces/hermes-profile.json

echo "== comparison exit codes =="
check "a real regression exits 3" 3 "$MPI" compare fixtures/runsets/baseline-android.json fixtures/runsets/candidate-android-regression.json --out "$TMP/cmp1.md"
check "no significant change exits 0" 0 "$MPI" compare fixtures/runsets/baseline-android.json fixtures/runsets/candidate-android-nochange.json --out "$TMP/cmp2.md"
check "a cross-platform pair exits 4, not 3" 4 "$MPI" compare fixtures/runsets/baseline-android.json fixtures/runsets/candidate-ios-crossplatform.json --out "$TMP/cmp3.md"
check_contains "the cross-platform refusal is explained" "cannot be used as a gate" \
  "$MPI" compare fixtures/runsets/baseline-android.json fixtures/runsets/candidate-ios-crossplatform.json

echo "== starting a simulator or emulator =="
# Listing is safe to run anywhere; booting is not, so only the refusals and
# the listing are asserted here.
check "boot --list succeeds" 0 "$MPI" boot --list
check_contains "an AVD name is not a device id" "not a device id" \
  "$MPI" boot
check "an unknown target is refused" 9 "$MPI" boot --device no-such-avd-or-udid
check_contains "and the refusal points at the listing" "boot --list" \
  "$MPI" boot --device no-such-avd-or-udid
check "a bad ready timeout is a usage error" 2 \
  "$MPI" boot --device whatever --ready-timeout-s 0

echo "== suppressions =="
# A suppression is a project decision, so the CLI and the desktop app read the
# same file. The three behaviours that make one auditable are asserted here.
cat > "$TMP/suppressions.json" <<'SUPP'
{
  "schema_version": "2.0",
  "suppressions": [
    {"rule_id": "DET-01", "reason": "known third-party surface, TICKET-42",
     "expiry": "2099-12-31", "author": "smoke-test"},
    {"rule_id": "DET-02", "reason": "lapsed on purpose", "expiry": "2020-01-01",
     "author": "smoke-test"},
    {"rule_id": "DET-04"}
  ]
}
SUPP
check_contains "a suppression with no reason is refused out loud" "not auditable" \
  "$MPI" analyze fixtures/traces/positive-frames-js-cpu.mpi.json --suppressions "$TMP/suppressions.json"
check_contains "an expired suppression is NOT applied and says so" "NOT applied because it had expired" \
  "$MPI" analyze fixtures/traces/positive-frames-js-cpu.mpi.json --suppressions "$TMP/suppressions.json"
check_contains "a live suppression applies with its reason retained" "TICKET-42" \
  "$MPI" analyze fixtures/traces/positive-frames-js-cpu.mpi.json --suppressions "$TMP/suppressions.json"
check_contains "a suppressed finding stays in the export" "Suppressed" \
  "$MPI" analyze fixtures/traces/positive-frames-js-cpu.mpi.json --suppressions "$TMP/suppressions.json"
check "a malformed suppression file is a usage error" 2 \
  "$MPI" analyze fixtures/traces/positive-frames-js-cpu.mpi.json --suppressions /dev/null

echo "== timeline =="
# The timeline's whole job is to keep "not measured" apart from "zero" once
# the data is shaped for drawing, so the legend that says so is asserted, not
# assumed.
check "timeline renders a trace" 0 "$MPI" timeline fixtures/traces/positive-frames-js-cpu.mpi.json --bins 30
check_contains "a blank column is labelled NOT MEASURED" "(blank) NOT MEASURED" \
  "$MPI" timeline fixtures/traces/positive-frames-js-cpu.mpi.json
check_contains "a covered empty bin is labelled a measured zero" "measured zero" \
  "$MPI" timeline fixtures/traces/positive-frames-js-cpu.mpi.json
check_contains "tracks carry what they do not say" "what these tracks do not say" \
  "$MPI" timeline fixtures/traces/positive-frames-js-cpu.mpi.json
check_contains "a synthetic fixture says so before any track" "SYNTHETIC" \
  "$MPI" timeline fixtures/traces/positive-frames-js-cpu.mpi.json
check_contains "an unmeasured bin is null in JSON, never 0" '"value": null' \
  "$MPI" timeline fixtures/traces/incomplete-evidence.mpi.json --json
check "an out-of-range bin count is refused" 2 "$MPI" timeline fixtures/traces/positive-frames-js-cpu.mpi.json --bins 99999
check "timeline without an argument is a usage error" 2 "$MPI" timeline

echo "== rules catalog =="
check "rules listing succeeds" 0 "$MPI" rules
for det in DET-01 DET-02 DET-03 DET-04 DET-05 DET-06 DET-07 DET-08 DET-09 DET-10 DET-11 DET-12; do
  check_contains "$det is registered" "$det" "$MPI" rules
done

echo "== record refuses rather than fabricating =="
check "record without --app is a usage error" 2 "$MPI" record --sessions-dir "$TMP/s"
if [ ! -d "$TMP/s" ]; then
  printf '  ok   a refused record creates no session directory\n'; pass=$((pass+1))
else
  printf '  FAIL a refused record left a session directory behind\n'; fail=$((fail+1))
fi

echo "== session package lifecycle =="
if "$MPI" record --app com.example.fixture --device no-such-device \
     --import fixtures/traces/negative-healthy.mpi.json \
     --sessions-dir "$TMP/sessions" >/dev/null 2>&1; then
  printf '  FAIL record accepted a nonexistent device\n'; fail=$((fail+1))
else
  printf '  ok   record rejects a nonexistent device\n'; pass=$((pass+1))
fi
check "export of a missing session is an error" 5 "$MPI" export "$TMP/nope"

echo "== translations =="
# A catalog keyed by English text cannot fail loudly: edit a sentence in a view
# and its translation silently stops applying. This is the check for that.
if python3 "$(dirname "$0")/check-i18n.py" --quiet; then
  printf '  ok   every translation still matches a rendered string\n'; pass=$((pass+1))
else
  printf '  FAIL the translation catalog has drifted from the views\n'; fail=$((fail+1))
fi


echo "== mcp server over stdio =="

# The dispatcher has unit tests; the transport does not, and the transport is
# where a host-visible regression lives. Three things are asserted here that
# no unit test can reach: that stdout carries only protocol, that a
# notification draws no reply, and that a mutating tool stays refused.

MCP_INIT='{"jsonrpc":"2.0","id":1,"method":"initialize"}'
MCP_NOTE='{"jsonrpc":"2.0","method":"notifications/initialized"}'
MCP_LIST='{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
MCP_BOOT='{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"boot_device","arguments":{"identifier":"x"}}}'

# Every line of stdout must be a JSON object, and there must be exactly two:
# a stray log line is a parse error at the host, which surfaces as "the server
# crashed on startup" with nothing saying why, and a reply to the notification
# would be a third.
check "stdout is exactly two JSON responses, and a notification draws none" 0 \
  sh -c "printf '%s\n%s\n%s\n' '$MCP_INIT' '$MCP_NOTE' '$MCP_LIST' \
         | '$MPI' mcp 2>/dev/null \
         | python3 -c 'import json,sys; ls=[l for l in sys.stdin if l.strip()]; [json.loads(l) for l in ls]; sys.exit(0 if len(ls)==2 else 1)'"

check_contains "initialize names the protocol revision" "2024-11-05" \
  sh -c "printf '%s\n' '$MCP_INIT' | '$MPI' mcp 2>/dev/null"

check_contains "tools/list offers the report tools" "read_session" \
  sh -c "printf '%s\n%s\n' '$MCP_INIT' '$MCP_LIST' | '$MPI' mcp 2>/dev/null"

check_contains "a read-only server refuses a mutating tool" "Nothing was done" \
  sh -c "printf '%s\n%s\n' '$MCP_INIT' '$MCP_BOOT' | '$MPI' mcp 2>/dev/null"

check_contains "and names the flag that would allow it" "allow-actions" \
  sh -c "printf '%s\n%s\n' '$MCP_INIT' '$MCP_BOOT' | '$MPI' mcp 2>/dev/null"

check_contains "a call before initialize is refused" "initialize must be called" \
  sh -c "printf '%s\n' '$MCP_LIST' | '$MPI' mcp 2>/dev/null"

check_contains "a line that is not JSON is a parse error, not a crash" "-32700" \
  sh -c "printf 'not json\n' | '$MPI' mcp 2>/dev/null"

echo "== app bundle =="
# The icon is generated at build time, so its absence is a build wiring
# failure rather than a missing file someone forgot to commit -- and a
# resource added after `codesign` runs leaves a bundle that does not verify
# against its own manifest. Both are checked here because both have happened.
BUNDLE="$(dirname "$MPI")/DevX.app"
if [ -d "$BUNDLE" ]; then
  if [ -f "$BUNDLE/Contents/Resources/DevX.icns" ]; then
    printf '  ok   the bundle carries a generated icon\n'; pass=$((pass+1))
  else
    printf '  FAIL the bundle has no Contents/Resources/DevX.icns\n'; fail=$((fail+1))
  fi
  if /usr/libexec/PlistBuddy -c 'Print :CFBundleIconFile' \
       "$BUNDLE/Contents/Info.plist" >/dev/null 2>&1; then
    printf '  ok   Info.plist names the icon\n'; pass=$((pass+1))
  else
    printf '  FAIL Info.plist has no CFBundleIconFile, so the icon is ignored\n'; fail=$((fail+1))
  fi
  if codesign --verify "$BUNDLE" >/dev/null 2>&1; then
    printf '  ok   the signed bundle still verifies with its resources\n'; pass=$((pass+1))
  else
    printf '  FAIL the bundle does not verify against its own signature\n'; fail=$((fail+1))
  fi
else
  printf '  skip DevX.app was not built (no Swift toolchain)\n'
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
