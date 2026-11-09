// Included ahead of every RommPS screen in the Steam Deck build (the PS5's
// ps5/src/prelude.h, without the Vulkan template): the standard headers the
// screens lean on, and the template's log and clock, which main.cpp gives.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef __cplusplus
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#define DECK_C extern "C"
#else
#define DECK_C
#endif

DECK_C void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
DECK_C double now_seconds(void);
