#pragma once
#include "config.h"
#if WITH_COMPANION
#include "Platform.h"
#include "Settings.h"
void companionWebBegin(WebServerClass& server, Settings& settings, bool (*requireAuth)());
#endif
