#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
# Any env's resolved libdeps work; CI points this at the one env it builds.
THEME_LIBDEPS=${THEME_LIBDEPS:-.pio/libdeps/smalltv_esp32_8mb}
THEME_JSON_INCLUDE=$THEME_LIBDEPS/ArduinoJson/src
c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc/features/theme -I"$THEME_JSON_INCLUDE" tests/theme/test_engine.cpp src/features/theme/ThemeEngine.cpp -o /tmp/smalltv-theme-tests
/tmp/smalltv-theme-tests
c++ -std=c++11 -Wall -Wextra -Werror -g -pthread -Isrc/features/theme -I"$THEME_JSON_INCLUDE" tests/theme/test_data.cpp src/features/theme/ThemeEngine.cpp -o /tmp/smalltv-theme-data-tests
/tmp/smalltv-theme-data-tests
c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc/features/theme -I"$THEME_JSON_INCLUDE" tests/theme/test_package.cpp src/features/theme/ThemePackage.cpp src/features/theme/ThemeEngine.cpp -o /tmp/smalltv-theme-package-tests
/tmp/smalltv-theme-package-tests
c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc/features/theme -I"$THEME_JSON_INCLUDE" tests/theme/test_render.cpp src/features/theme/ThemeEngine.cpp -o /tmp/smalltv-theme-render-tests
/tmp/smalltv-theme-render-tests
c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc/features/theme -I"$THEME_JSON_INCLUDE" tests/theme/test_validation.cpp src/features/theme/ThemeEngine.cpp -o /tmp/smalltv-theme-validation-tests
/tmp/smalltv-theme-validation-tests
c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc/features/theme -I"$THEME_JSON_INCLUDE" -I"$THEME_LIBDEPS/GFX Library for Arduino/src" tests/theme/check_example.cpp src/features/theme/ThemePackage.cpp src/features/theme/ThemeEngine.cpp -o /tmp/smalltv-theme-example-check
/tmp/smalltv-theme-example-check examples/themes/pixel-room.stheme
/tmp/smalltv-theme-example-check examples/themes/terminal-ops.stheme
c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc/features/theme -I"$THEME_JSON_INCLUDE" tests/theme/test_catalog.cpp src/features/theme/ThemePackage.cpp src/features/theme/ThemeEngine.cpp -o /tmp/smalltv-theme-catalog-tests
/tmp/smalltv-theme-catalog-tests
