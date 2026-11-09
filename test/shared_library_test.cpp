// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The headers define the category of the c-ares status codes, so every shared library that uses
// them has a copy of its object -- and a code that comes out of one of them has to compare equal
// to the same code made anywhere else all the same.
//
#include "shared_library.hpp"

#include <c_ares_async/error.hpp>

#include <ares.h>

#include <gtest/gtest.h>

TEST(SharedLibrary, HasTheSameCategory)
{
   EXPECT_TRUE(ares_category_of_shared_library() == c_ares_async::ares_category());
}

TEST(SharedLibrary, MakesTheSameCodes)
{
   EXPECT_EQ(ares_error_of_shared_library(ARES_ENOTFOUND),
             c_ares_async::make_ares_error(ARES_ENOTFOUND));
   EXPECT_NE(ares_error_of_shared_library(ARES_ENOTFOUND),
             c_ares_async::make_ares_error(ARES_ETIMEOUT));
}
