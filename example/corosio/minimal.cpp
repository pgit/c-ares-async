// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The shortest program that resolves a name, on Corosio: the addresses of a host, and then
// its HTTPS record, each awaited in a coroutine. ../asio/minimal.cpp is the same on Asio.
//
//    minimal [host]
//
#include <c_ares_async.hpp>
#include <c_ares_async/corosio/formatter.hpp>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>
#include "boost/capy/ex/any_executor.hpp"

#include <exception>
#include <format>
#include <iostream>
#include <memory_resource>
#include <string>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

static c_ares_async::Task<void> lookup(std::string host)
{
   auto executor = co_await capy::this_coro::executor;
   c_ares_async::resolver resolver(executor);

   const auto [ec, endpoints] = co_await resolver.resolve(host, "443");

   if (ec)
      std::cerr << host << ": " << ec.message() << '\n';

   for (const auto& endpoint : endpoints)
      std::cout << std::format("{}: {}\n", host, endpoint);

   //
   // Most names have no HTTPS record, which is no error worth a mention.
   //
   const auto [https_ec, records] = co_await resolver.resolve_https(host);

   if (https_ec && https_ec != c_ares_async::make_ares_error(ARES_ENODATA))
      std::cerr << host << ": HTTPS: " << https_ec.message() << '\n';

   for (const auto& record : records)
   {
      std::cout << host << ": HTTPS " << record.priority << ' '
                << (record.target.empty() ? "." : record.target);
      for (const auto& param : record.params)
         std::cout << ' ' << param;
      std::cout << '\n';
   }
}

int main(int argc, char* argv[])
{
   corosio::io_context context;

   //
   // The executor is handed to the coroutine rather than taken from inside it: what
   // capy::this_coro::executor yields does not own its context, and the resolver keeps the one
   // it was given.
   //
   std::pmr::memory_resource* mr = context.get_frame_allocator();
   // mr = std::pmr::new_delete_resource();
   capy::run_async(
      context.get_executor(), mr, []() {},
      [](std::exception_ptr) noexcept {})(lookup(argc > 1 ? argv[1] : "example.com"));

   context.run();
}
