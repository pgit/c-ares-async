// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// A std::formatter for Asio's endpoints, which have an output operator but nothing for
// std::format. This is anyhttp's (include/anyhttp/formatter.hpp there), as far as this library
// has a use for it. corosio/formatter.hpp is the same for Corosio's endpoint.
//
// It is not included by c_ares_async.hpp: a specialization of std::formatter for a type of
// Asio's is for the program to have, once, and it may have one already.
//
#pragma once

#include <boost/asio/ip/basic_endpoint.hpp>

#include <format>

/** "address:port", with an IPv6 address in square brackets, as the output operator has it. */
template <class Protocol>
struct std::formatter<boost::asio::ip::basic_endpoint<Protocol>>
{
   constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

   template <class FormatContext>
   auto format(const boost::asio::ip::basic_endpoint<Protocol>& endpoint, FormatContext& ctx) const
   {
      const auto address = endpoint.address();
      return address.is_v6()
                ? std::format_to(ctx.out(), "[{}]:{}", address.to_string(), endpoint.port())
                : std::format_to(ctx.out(), "{}:{}", address.to_string(), endpoint.port());
   }
};
