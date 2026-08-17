#!/usr/bin/env bash
# Proves the host's plugin contract against a stand-in game: priority ordering,
# event consumption, ABI rejection, and silence outside the game.
set -uo pipefail
B="${1:-build}"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
fails=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

check() { # name expected actual
  if [ "$2" = "$3" ]; then
    echo "PASS  $1"
  else
    echo "FAIL  $1 — expected '$2', got '$3'"
    fails=$((fails + 1))
  fi
}

LOG="$TMP/d.log"
out=$(env BG3LE_LOG="$LOG" BG3LE_FORCE_HOST=1 LD_PRELOAD=./"$B"/test_dispatch.so \
      ./"$B"/harness)

echo "=== host dispatch ==="

# X is eaten by "early", Y by "late", Z by nobody.
check "only the unconsumed keys reach the game" "RESULT keys=2" \
      "$(grep '^RESULT' <<<"$out")"
check "and the right one got through" "1" "$(grep -c 'GAME SAW Z' <<<"$out")"

# Injection: Q is swallowed, R arrives in its place.
check "the swapped-out key never reaches the game" "0" "$(grep -c 'GAME SAW Q' <<<"$out")"
check "the injected key does" "1" "$(grep -c 'GAME SAW R' <<<"$out")"
check "injection ran once, not in a loop" "1" "$(grep -c 'swapped Q for R' "$LOG")"
check "injected events are not re-dispatched" "0" "$(grep -cE '(early|late): saw R' "$LOG")"

# Priority, not link order: "late" is declared first in the source.
order=$(grep -oE '(early|late): init' "$LOG" | head -2 | cut -d: -f1 | tr '\n' ' ')
check "plugins init in priority order" "early late " "$order"

# A consumed event must not be offered to later plugins.
check "early saw X" "1" "$(grep -c 'early: saw X' "$LOG")"
check "late never saw X" "0" "$(grep -c 'late: saw X' "$LOG")"
check "early saw Y but did not consume it" "1" "$(grep -c 'early: saw Y' "$LOG")"
check "late saw Y" "1" "$(grep -c 'late: saw Y' "$LOG")"
check "both saw Z" "2" "$(grep -cE '(early|late): saw Z' "$LOG")"

# The frame tick fires once the game drains its queue.
check "on_frame ran" "1" "$(grep -c 'early: first frame' "$LOG")"

# A duplicate name is refused, not run twice.
check "a duplicate plugin name is refused" "1" \
      "$(grep -c "plugin 'dupe' is already loaded" "$LOG")"
check "and it initialises exactly once" "1" "$(grep -c 'dupe: init' "$LOG")"

# A plugin built against another ABI is refused, not loaded and crossed fingers.
check "wrong-ABI plugin refused" "1" "$(grep -c "plugin 'stale' built against ABI" "$LOG")"

# Patches are the host's to undo, and there were none here.
check "no patches left behind" "0" "$(grep -c 'restored .* patch' "$LOG")"

echo
echo "=== outside the game ==="
# Steam re-execs through many helpers; loading into one must be silent.
: > "$TMP/quiet.log"
BG3LE_LOG="$TMP/quiet.log" LD_PRELOAD=./"$B"/test_dispatch.so ./"$B"/harness >/dev/null
check "silent in a non-game process" "0" "$(wc -l < "$TMP/quiet.log")"

: > "$TMP/loud.log"
BG3LE_LOG="$TMP/loud.log" BG3LE_VERBOSE=1 LD_PRELOAD=./"$B"/test_dispatch.so \
  ./"$B"/harness >/dev/null 2>/dev/null
check "BG3LE_VERBOSE=1 says why it went idle" "1" \
      "$(grep -c 'not Baldur' "$TMP/loud.log")"
check "no plugin runs outside the game" "0" "$(grep -c 'saw ' "$TMP/loud.log")"

echo
if [ "$fails" -eq 0 ]; then echo "ALL PASSED"; else echo "FAILED ($fails)"; fi
exit $((fails != 0))
