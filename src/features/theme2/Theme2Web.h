#pragma once
#include "config.h"
#if WITH_THEME2
#include "Platform.h"
#include "Settings.h"
void theme2WebBegin(WebServerClass& web, Settings& s, bool (*requireAuth)());
#endif
