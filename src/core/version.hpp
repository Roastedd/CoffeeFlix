// This build's version, as the About card shows it and the updater compares it with releases.
// The build writes app_version.h: the release tag on CI (make APP_VERSION=2.2.0), otherwise
// `git describe` (2.1.2-11-gf25ba75: eleven commits after 2.1.2).
#pragma once

#if __has_include("app_version.h")
#include "app_version.h"
#endif
#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
