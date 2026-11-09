// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#include "asio_lookup.hpp"

#include <c_ares_async/resolver.hpp>

#include <boost/asio/io_context.hpp>

static_assert(C_ARES_ASYNC_ASIO, "asio_lookup.cpp is the part of the tests that is on Asio");

std::vector<std::string> resolve_on_asio(const std::string& host, const std::string& service)
{
   boost::asio::io_context context;
   c_ares_async::resolver resolver(context.get_executor());

   std::vector<std::string> found;
   resolver.async_resolve(
      host, service,
      [&](c_ares_async::error_code ec, const c_ares_async::resolver::results_type& endpoints) {
         if (ec)
            found.push_back(ec.message());
         for (const auto& endpoint : endpoints)
            found.push_back(endpoint.address().to_string() + ':' + std::to_string(endpoint.port()));
      });
   context.run();
   return found;
}
