#!/usr/bin/env bash
# ============================================================================
# mllvm -- test suite
# ----------------------------------------------------------------------------
# Every .ll file in tests/cases is a program.  <name>.exit is the expected
# exit status and <name>.out the expected standard output (empty when the
# file does not exist).  Each program is run twice: directly and with the
# constant folder enabled.
#
# The files in tests/cases/bad must be rejected by "mllvm check".
# ============================================================================
set -u
cd "$(dirname "$0")/.."

BIN="${BIN:-./mllvm}"
[ -x "$BIN" ] || { echo "error: $BIN is not built" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0

ok()   { pass=$((pass + 1)); printf 'ok   %s\n' "$1"; }
nok()  { fail=$((fail + 1)); printf 'FAIL %s\n' "$1"; }

for f in tests/cases/*.ll; do
  name="$(basename "$f" .ll)"
  exp_exit=0
  [ -f "tests/cases/$name.exit" ] && exp_exit="$(cat "tests/cases/$name.exit")"
  exp_out=""
  [ -f "tests/cases/$name.out" ] && exp_out="$(cat "tests/cases/$name.out")"

  for mode in plain opt; do
    if [ "$mode" = opt ]; then
      "$BIN" run --opt "$f" >"$tmp/out" 2>"$tmp/err"
    else
      "$BIN" run "$f" >"$tmp/out" 2>"$tmp/err"
    fi
    code=$?
    got_out="$(cat "$tmp/out")"
    if [ "$code" != "$exp_exit" ]; then
      nok "$name ($mode): exit $code, expected $exp_exit"
      sed 's/^/     /' "$tmp/err"
    elif [ "$got_out" != "$exp_out" ]; then
      nok "$name ($mode): stdout differs"
      printf '     expected: %q\n     got:      %q\n' "$exp_out" "$got_out"
    else
      ok "$name ($mode)"
    fi
  done

  if "$BIN" check "$f" >/dev/null 2>"$tmp/err"; then
    ok "$name (check)"
  else
    nok "$name (check)"
    sed 's/^/     /' "$tmp/err"
  fi
done

for f in tests/cases/bad/*.ll; do
  name="bad/$(basename "$f" .ll)"
  if "$BIN" check "$f" >/dev/null 2>&1; then
    nok "$name: invalid IR was accepted"
  else
    ok "$name"
  fi
done

# The printer has to emit IR that the parser accepts again, and the folded
# constants have to be visible in that output.
"$BIN" opt tests/cases/fold.ll >"$tmp/folded.ll" 2>"$tmp/err"
opt_code=$?
"$BIN" run "$tmp/folded.ll" >/dev/null 2>>"$tmp/err"
round_code=$?
if [ "$opt_code" -eq 0 ] &&
   grep -q '%a = add i32 42, 0' "$tmp/folded.ll" &&
   [ "$round_code" -eq 42 ]; then
  ok "opt: folded IR round-trips"
else
  nok "opt: folded IR round-trips"
  sed 's/^/     /' "$tmp/err"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
