// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// What the headers make of a c-ares status code inside a shared library of their own, which is
// compiled with -fvisibility=hidden and shows nothing but these two. See shared_library_test.cpp.
//
#pragma once

#include <c_ares_async/error.hpp>

[[gnu::visibility("default")]] c_ares_async::error_code ares_error_of_shared_library(int status);

[[gnu::visibility("default")]] const c_ares_async::error_category&
ares_category_of_shared_library();
