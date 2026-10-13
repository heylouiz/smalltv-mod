#!/bin/sh
# Theme v2 parity tests: JS writes fixtures, the C++ core must match them.
set -eu
cd "$(dirname "$0")/../.."
OUT=${THEME2_FIXTURES:-/tmp/smalltv-theme2-fixtures}
node tests/theme2/gen.mjs "$OUT"
c++ -std=c++11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Isrc/features/theme2 \
  tests/theme2/test_stv2.cpp src/features/theme2/Stv2.cpp src/features/theme2/Stv2Render.cpp src/features/theme2/Stv2Json.cpp \
  -o /tmp/smalltv-theme2-tests
/tmp/smalltv-theme2-tests "$OUT"
