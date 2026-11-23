// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// corosio/this_coro_executor.cpp on Asio: a coroutine starts work on its executor, stops it, and
// completes without waiting for it. The numbers are the order structured concurrency would give;
// the stopped work prints "1 finished" last. That is harmless here, as the executor is a copy that
// refers to the io_context alone, but not with the signal in the frame (the commented-out line):
// ASan then reports a bad free when the wait completes.
//
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>

#include <iostream>

namespace asio = boost::asio;

static asio::awaitable<void> this_coro()
{
   auto executor(co_await asio::this_coro::executor);
   auto signal = std::make_shared<asio::cancellation_signal>();
   // asio::cancellation_signal theSignal, *signal = &theSignal;
   asio::co_spawn(
      executor,
      []() -> asio::awaitable<void> {
         asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::hours(1));
         co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
         std::cout << "1 finished\n";
      }(),
      asio::bind_cancellation_slot(signal->slot(), [signal](const std::exception_ptr&) {}));
   signal->emit(asio::cancellation_type::terminal);
   // co_await post(executor);
   co_return;
}

int main()
{
   asio::io_context context;

   asio::co_spawn(context, this_coro(), [](const std::exception_ptr&) { //
      std::cout << "2 coro done\n";
   });

   context.run();
   std::cout << "3 done\n";
}
