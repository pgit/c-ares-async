// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#include "shared_library.hpp"

c_ares_async::error_code ares_error_of_shared_library(int status)
{
   return c_ares_async::make_ares_error(status);
}

const c_ares_async::error_category& ares_category_of_shared_library()
{
   return c_ares_async::ares_category();
}
