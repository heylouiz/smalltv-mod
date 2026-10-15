#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc/features/companion tests/companion/test_proto.cpp src/features/companion/CompanionProto.cpp -o /tmp/smalltv-companion-tests
/tmp/smalltv-companion-tests
