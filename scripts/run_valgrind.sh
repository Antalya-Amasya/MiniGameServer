#!/bin/bash
set -u
cd "$(dirname "$0")/../build-linux"
for name in minigame_tests minigame_integration minigame_reactor_tests minigame_phase3_tests minigame_phase5_limits minigame_phase4_tests; do
  echo "==== ${name}"
  valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
    --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
    "./bin/${name}" >"/tmp/vg-${name}.out" 2>"/tmp/vg-${name}.err"
  echo "EXIT:$?"
  grep -E "ERROR SUMMARY|definitely lost|indirectly lost|still reachable" "/tmp/vg-${name}.err" || true
done
echo "VALGRIND_DONE"
