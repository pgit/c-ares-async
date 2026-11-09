// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// Both runtimes in one program. The library is headers, so each translation unit compiles it for
// the runtime it is on -- here Corosio, and Asio in asio_lookup.cpp -- and the two have to stay
// apart: the inline namespace that each runtime's definitions are in is what keeps the linker
// from taking one of them for both. resolver::~resolver() and set_logger() would otherwise be the
// same symbol on either, with the members of the other runtime's resolver behind it.
//
// Addresses are taken as they are, without a query, so neither lookup needs a server.
//
#include "asio_lookup.hpp"

#include <c_ares_async/resolver.hpp>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <exception>
#include <string>
#include <vector>

using testing::ElementsAre;

static_assert(C_ARES_ASYNC_COROSIO, "this is the part of the tests that is on Corosio");

namespace
{

c_ares_async::Task<void> lookup(c_ares_async::resolver& resolver, std::string host,
                                std::string service, std::vector<std::string>& found)
{
   auto [ec, endpoints] = co_await resolver.resolve(std::move(host), std::move(service));
   if (ec)
      found.push_back(ec.message());
   for (const auto& endpoint : endpoints)
      found.push_back(endpoint.address().to_string() + ':' + std::to_string(endpoint.port()));
}

std::vector<std::string> resolve_on_corosio(const std::string& host, const std::string& service)
{
   boost::corosio::io_context context;
   const c_ares_async::Executor executor(context.get_executor());
   c_ares_async::resolver resolver(executor);

   std::vector<std::string> found;
   boost::capy::run_async(
      executor, []() noexcept {},
      [](std::exception_ptr) noexcept {})(lookup(resolver, host, service, found));
   context.run();
   return found;
}

} // namespace

TEST(MixedRuntimes, BothResolveInOneProgram)
{
   EXPECT_THAT(resolve_on_asio("192.0.2.7", "443"), ElementsAre("192.0.2.7:443"));
   EXPECT_THAT(resolve_on_corosio("192.0.2.8", "853"), ElementsAre("192.0.2.8:853"));

   //
   // And the other way round, in case whichever goes first is what both get.
   //
   EXPECT_THAT(resolve_on_corosio("2001:db8::1", "53"), ElementsAre("2001:db8::1:53"));
   EXPECT_THAT(resolve_on_asio("2001:db8::2", "53"), ElementsAre("2001:db8::2:53"));
}
