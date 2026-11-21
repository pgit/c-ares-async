// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The shortest program that resolves a name: the addresses of a host, awaited in a coroutine.
//
//    minimal [host]
//
// The resolver stops its work when it goes, without waiting for it, so that work can finish after
// lookup() has. That is harmless here, as the executor is a copy that refers to the io_context
// alone. The same, with nothing of c-ares-async in it: ../../test/asio/this_coro_executor.cpp.
//
#include <c_ares_async.hpp>
#include <c_ares_async/asio/formatter.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <format>
#include <iostream>
#include <string>

namespace asio = boost::asio;

asio::awaitable<void> lookup(std::string host)
{
   c_ares_async::resolver resolver(co_await asio::this_coro::executor);

   const auto [ec, endpoints] = co_await resolver.resolve(host, "443");
   co_await resolver.async_resolve(host, "443", asio::as_tuple(asio::use_awaitable));

   if (ec)
      std::cerr << host << ": " << ec.message() << '\n';

   for (const auto& endpoint : endpoints)
      std::cout << std::format("{}: {}\n", host, endpoint);
}

int main(int argc, char* argv[])
{
   asio::io_context context;
   asio::co_spawn(context, lookup(argc > 1 ? argv[1] : "example.com"), asio::detached);
   context.run();
}
