// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// A std::formatter for Corosio's endpoint, which has neither an output operator nor anything for
// std::format. This is anyhttp's (include/anyhttp/formatter.hpp there), as far as this library
// has a use for it. asio/formatter.hpp is the same for Asio's endpoints.
//
// It is not included by c_ares_async.hpp: a specialization of std::formatter for a type of
// Corosio's is for the program to have, once, and it may have one already.
//
#pragma once

#include <boost/corosio/endpoint.hpp>

#include <format>

/** "address:port", with an IPv6 address in square brackets, as Asio has it. */
template <>
struct std::formatter<boost::corosio::endpoint>
{
   constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

   template <class FormatContext>
   auto format(const boost::corosio::endpoint& endpoint, FormatContext& ctx) const
   {
      const auto address = endpoint.address();
      return address.is_v6()
                ? std::format_to(ctx.out(), "[{}]:{}", address.to_string(), endpoint.port())
                : std::format_to(ctx.out(), "{}:{}", address.to_string(), endpoint.port());
   }
};
