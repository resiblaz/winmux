// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#define BLOCK_TIL
#include "LibraryIncludes.h"

#include <WexTestClass.h>
#include "consoletaeftemplates.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
