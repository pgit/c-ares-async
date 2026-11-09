// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// What asio.cpp is, on Corosio: enough of the library to need its headers and those of its
// dependencies, and nothing that needs a network -- an address is resolved without asking anybody.
//
#include <c_ares_async.hpp>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>

static c_ares_async::Task<void> lookup(c_ares_async::resolver& resolver, bool& resolved)
{
   const auto [ec, endpoints] = co_await resolver.resolve("127.0.0.1", "443");
   resolved = !ec && endpoints.size() == 1 && endpoints.front().address().is_loopback() &&
              endpoints.front().port() == 443;
}

int main()
{
   boost::corosio::io_context context;
   const c_ares_async::Executor executor(context.get_executor());
   c_ares_async::resolver resolver(executor);

   bool resolved = false;
   boost::capy::run_async(
      executor, []() noexcept {}, [](std::exception_ptr) noexcept {})(lookup(resolver, resolved));
   context.run();

   std::cout << "c_ares_async " << C_ARES_ASYNC_VERSION_STRING
             << " on Corosio: " << (resolved ? "resolved" : "FAILED") << '\n';

   return resolved ? EXIT_SUCCESS : EXIT_FAILURE;
}
