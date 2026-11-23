// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// A coroutine starts work on the executor capy::this_coro::executor yields, stops it, and
// completes without waiting for it. The numbers are the order structured concurrency would give;
// the stopped work prints "1 finished" last, after the frame its executor_ref points into is gone.
//
//    this_coro_executor           frames from new_delete_resource(): heap-use-after-free in ASan
//    this_coro_executor pooled    the context's frame allocator: quiet, the freed frame is reused
//
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/capy/task.hpp>

#include <boost/corosio/delay.hpp>
#include <boost/corosio/io_context.hpp>

#include <iostream>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

static capy::task<void> this_coro()
{
   auto executor(co_await capy::this_coro::executor);
   std::stop_source stop;
   capy::run_async(executor, stop.get_token())([]() -> capy::task<void> { //
      std::ignore = co_await corosio::delay(std::chrono::hours(1));
      std::cout << "1 finished\n";
   }());
   stop.request_stop();
   co_return;
}

int main(int argc, char* argv[])
{
   corosio::io_context context;

   std::pmr::memory_resource* mr;
   if (argc > 1 && std::string_view(argv[1]) == "pooled")
      mr = context.get_frame_allocator();
   else
      mr = std::pmr::new_delete_resource();

   capy::run_async(context.get_executor(), mr, []() { //
      std::cout << "2 coro done\n";
   })(this_coro());

   context.run();
   std::cout << "3 done" << '\n';
}
