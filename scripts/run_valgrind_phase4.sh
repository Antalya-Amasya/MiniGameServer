#!/bin/bash
set -u
cd "$(dirname "$0")/../build-linux"
valgrind --leak-check=full --show-leak-kinds=all \
  --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
  ./bin/minigame_phase4_tests >vg-phase4.out 2>vg-phase4.err
echo "PHASE4_VG_EXIT:$?"
grep -E "FAIL|exception|phase4 checks|definitely lost|ERROR SUMMARY" vg-phase4.out vg-phase4.err
