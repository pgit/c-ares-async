// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The version of the library, which is where CMakeLists.txt reads it from as well: this is the one
// place that says it, for a consumer of the headers that has no CMake package to ask.
//
#pragma once

#define C_ARES_ASYNC_VERSION_MAJOR 0
#define C_ARES_ASYNC_VERSION_MINOR 1
#define C_ARES_ASYNC_VERSION_PATCH 0

// "major.minor.patch"
#define C_ARES_ASYNC_VERSION_STRING "0.1.0"

// major * 10000 + minor * 100 + patch, for comparing in the preprocessor
#define C_ARES_ASYNC_VERSION                                                                       \
   (C_ARES_ASYNC_VERSION_MAJOR * 10000 + C_ARES_ASYNC_VERSION_MINOR * 100 +                        \
    C_ARES_ASYNC_VERSION_PATCH)
