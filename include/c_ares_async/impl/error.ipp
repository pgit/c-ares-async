// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <c_ares_async/error.hpp> // NOLINT(misc-header-include-cycle)

#include <ares.h>

#include <string>

namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME
{

namespace detail
{

//
// The id is for Boost.System, see error_category_base, and a random number like any other: one
// that is in use elsewhere would make the two categories one.
//
class ares_error_category final : public error_category_base<0x995aab04fc421592>
{
public:
   const char* name() const noexcept override { return "c-ares"; }
   std::string message(int value) const override { return ares_strerror(value); }
};

} // namespace detail

inline const error_category& ares_category() noexcept
{
   static const detail::ares_error_category category;
   return category;
}

inline error_code make_ares_error(int status) noexcept
{
   return {status, ares_category()}; // ARES_SUCCESS is 0, so this stays falsy on success
}

} // namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME
