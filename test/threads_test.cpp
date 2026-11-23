// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// Resolvers on contexts that more than one thread runs, on either runtime. A resolver is an I/O
// object like any other: it is for one thread at a time, which a strand sees to where its context
// runs on several, and any number of them may be busy at the same time, on whichever threads.
//
// That nothing crashed says little about threads. What these tests look at is where things run:
// the coroutines that wait for a lookup, and the resolver's own work, which is what it logs from.
// Both have to be on the executor that the resolver was given, every time. The thread sanitizer
// of the tsan preset sees to the rest.
//
#include "fake_dns_server.hpp"
#include "test_certificate.hpp"

#include <c_ares_async/resolver.hpp>

#if C_ARES_ASYNC_COROSIO
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/strand.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ip_address.hpp>

#include <system_error>
#else
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#endif

#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using c_ares_async::Task;
using transport = fake_dns_server::transport;

namespace
{

// --------------------------------------------------------------------------------------------------
//
// What the two runtimes spell differently: the context, a strand of it, starting a coroutine on an
// executor with its result to come back as a future, and an endpoint to compare with.
//
#if C_ARES_ASYNC_COROSIO

using Context = boost::corosio::io_context;

auto make_strand(Context& context) { return boost::capy::strand(context.get_executor()); }

template <typename Executor, typename T>
std::future<T> spawn(const Executor& executor, Task<T> task)
{
   auto promise = std::make_shared<std::promise<T>>();
   auto future = promise->get_future();
   boost::capy::run_async(
      executor, [promise](T value) noexcept { promise->set_value(std::move(value)); },
      [promise](std::exception_ptr error) noexcept { promise->set_exception(error); })(
      std::move(task));

   return future;
}

c_ares_async::TcpEndpoint endpoint(std::string_view address, unsigned short port)
{
   auto [ec, parsed] = boost::corosio::make_ip_address(address);
   if (ec)
      throw std::system_error(ec, "make_ip_address");

   return {parsed, port};
}

#else

using Context = boost::asio::io_context;

auto make_strand(Context& context) { return boost::asio::make_strand(context); }

template <typename Executor, typename T>
std::future<T> spawn(const Executor& executor, Task<T> task)
{
   return boost::asio::co_spawn(executor, std::move(task), boost::asio::use_future);
}

c_ares_async::TcpEndpoint endpoint(std::string_view address, unsigned short port)
{
   return {boost::asio::ip::make_address(address), port};
}

#endif

// --------------------------------------------------------------------------------------------------

constexpr int names = 256; // in the zone: host0.example.test to host255.example.test
constexpr int threads = 4; // that run a context, where there is one for all of them

std::string host(int i) { return std::format("host{}.example.test", i); }
std::string address(int i) { return std::format("192.0.2.{}", i); }

//
// What a coroutine that looks up names found.
//
struct tally
{
   int resolved = 0; // names that came back as the zone has them, address and HTTPS record
   int elsewhere = 0; // lookups after which the coroutine was not resumed on its executor

   tally& operator+=(const tally& other)
   {
      resolved += other.resolved;
      elsewhere += other.elsewhere;
      return *this;
   }
};

//
// Looks up 'count' names from 'first' on, each for its address and then for its HTTPS record.
// The coroutine runs on 'executor', and has to be back on it after every lookup.
//
template <typename Executor>
Task<tally> look_up(c_ares_async::resolver& resolver, Executor executor, int first, int count)
{
   tally result;
   for (int i = first; i < first + count; ++i)
   {
      const auto [ec, endpoints] = co_await resolver.resolve(host(i), "443");
      result.elsewhere += executor.running_in_this_thread() ? 0 : 1;

      const auto [https_ec, records] = co_await resolver.resolve_https(host(i));
      result.elsewhere += executor.running_in_this_thread() ? 0 : 1;

      if (!ec && endpoints.size() == 1 && endpoints.front() == endpoint(address(i), 443) &&
          !https_ec && records.size() == 1)
         ++result.resolved;
   }

   co_return result;
}

//
// Runs 'context' on 'threads' threads until it runs out of work, which is once every coroutine
// started on it is through and every resolver made on it is gone. False where that takes longer
// than 'limit'.
//
[[nodiscard]] bool run(Context& context, std::chrono::seconds limit = std::chrono::seconds(30))
{
   std::vector<std::thread> pool;
   for (int i = 0; i < threads; ++i)
      pool.emplace_back([&] { context.run_for(limit); });

   for (auto& thread : pool)
      thread.join();

   return context.stopped();
}

} // namespace

// --------------------------------------------------------------------------------------------------

//
// The DNS server has a context and a thread of its own, and is asked over UDP or over TLS,
// whichever the test is for.
//
class Threads : public testing::TestWithParam<transport>
{
protected:
   void SetUp() override
   {
      for (int i = 0; i < names; ++i)
      {
         server.add_address(host(i), address(i));
         server.add_https(host(i), 1, ".", {{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}}});
      }

      //
      // Without verifying the server's certificate, which would take a trust store of its own:
      // what is to be seen here is the TLS sessions of many sockets on many threads.
      //
      if (GetParam() == transport::tls)
      {
         static const auto certificate = make_test_certificate("dns.example.test");
         server.use_tls(certificate);
         tls = {.enabled = true, .verify = false};
      }

      //
      // Nothing may be asked of the server once its thread runs, as that would race it.
      //
      servers = server.address();
      server_thread = std::thread([this] { server_context.run(); });
   }

   void TearDown() override { stop_server(); }

   //
   // Stops the server's thread, after which the server can be asked what it was asked.
   //
   void stop_server()
   {
      server_context.stop();
      if (server_thread.joinable())
         server_thread.join();
   }

   //
   // What every test expects in the end: all the names resolved, on the transport of the test,
   // and nothing resumed or done anywhere but on the executor it belongs on.
   //
   void expect_all_resolved(const tally& total)
   {
      EXPECT_EQ(total.resolved, names);
      EXPECT_EQ(total.elsewhere, 0);

      EXPECT_GT(messages, 0);
      EXPECT_EQ(messages_elsewhere, 0);

      stop_server();
      EXPECT_EQ(server.count(GetParam()), server.queries().size());
   }

   //
   // A logger for a resolver on 'executor', which counts the messages and those that were not
   // logged from inside the executor. The resolver logs from wherever it is at work, in its socket
   // and timer callbacks as well as in what its callers call, so this is where a test sees that.
   //
   template <typename Executor>
   c_ares_async::log_function logger(Executor executor)
   {
      return [this, executor](c_ares_async::log_level, std::string_view) {
         ++messages;
         if (!executor.running_in_this_thread())
            ++messages_elsewhere;
      };
   }

   //
   // Makes a resolver on 'executor', has it look up 'count' names from 'first' on, and destroys it
   // again, all of it in a coroutine that runs on 'executor', too.
   //
   template <typename Executor>
   Task<tally> make_and_use(Executor executor, int first, int count)
   {
      c_ares_async::resolver resolver(executor, tls);
      if (resolver.set_servers(servers))
         co_return tally{};

      resolver.set_logger(logger(executor));
      co_return co_await look_up(resolver, executor, first, count);
   }

   c_ares_async::tls_options tls;
   std::string servers;

   std::atomic<int> messages = 0;
   std::atomic<int> messages_elsewhere = 0;

   boost::asio::io_context server_context;
   fake_dns_server server{server_context.get_executor()};
   std::thread server_thread;
};

INSTANTIATE_TEST_SUITE_P(Transports, Threads, testing::Values(transport::udp, transport::tls),
                         [](const testing::TestParamInfo<transport>& info) {
                            return info.param == transport::tls ? "OverTls" : "OverUdp";
                         });

// --------------------------------------------------------------------------------------------------

TEST_P(Threads, ShareOneResolverThroughAStrand)
{
   //
   // One resolver on a strand of a context that four threads run, made before the threads start.
   // Eight coroutines on that strand have their lookups in flight at the same time, and the last
   // one through destroys the resolver, in the strand as well.
   //
   Context context;
   const auto strand = make_strand(context);

   auto resolver = std::make_unique<c_ares_async::resolver>(strand, tls);
   ASSERT_FALSE(resolver->set_servers(servers));
   resolver->set_logger(logger(strand));

   constexpr int workers = 8;
   int running = workers; // only ever touched in the strand

   const auto worker = [&](int first, int count) -> Task<tally> {
      const auto result = co_await look_up(*resolver, strand, first, count);
      if (--running == 0)
         resolver.reset();

      co_return result;
   };

   std::vector<std::future<tally>> results;
   for (int i = 0; i < workers; ++i)
      results.push_back(spawn(strand, worker(i * names / workers, names / workers)));

   ASSERT_TRUE(run(context));

   tally total;
   for (auto& result : results)
      total += result.get();

   EXPECT_EQ(resolver, nullptr);
   expect_all_resolved(total);
}

TEST_P(Threads, UseManyResolversOnOneContextAtOnce)
{
   //
   // Sixteen resolvers on a context that four threads run, each on a strand of its own and made,
   // used and destroyed in a coroutine there. So they are made and destroyed on different threads
   // at the same time as well, which is where c-ares' library initialization would race, had the
   // resolver any part in it.
   //
   Context context;

   constexpr int resolvers = 16;
   std::vector<std::future<tally>> results;
   for (int i = 0; i < resolvers; ++i)
   {
      const auto strand = make_strand(context);
      results.push_back(
         spawn(strand, make_and_use(strand, i * names / resolvers, names / resolvers)));
   }

   ASSERT_TRUE(run(context));

   tally total;
   for (auto& result : results)
      total += result.get();

   expect_all_resolved(total);
}

TEST_P(Threads, UseAResolverEachOnContextsOfTheirOwn)
{
   //
   // The other way of putting threads to work: a context for every thread, run by that thread
   // alone, with a resolver of its own. Nothing needs a strand here.
   //
   std::vector<std::optional<tally>> results(threads);

   std::vector<std::thread> pool;
   for (int i = 0; i < threads; ++i)
      pool.emplace_back([&, i] {
         Context context;
         auto result =
            spawn(context.get_executor(),
                  make_and_use(context.get_executor(), i * names / threads, names / threads));

         context.run_for(std::chrono::seconds(30));
         if (!context.stopped())
            return;

         try
         {
            results[i] = result.get();
         }
         catch (const std::exception& error)
         {
            ADD_FAILURE() << error.what();
         }
      });

   for (auto& thread : pool)
      thread.join();

   tally total;
   for (const auto& result : results)
   {
      ASSERT_TRUE(result.has_value()) << "a context did not run out of work in time";
      total += *result;
   }

   expect_all_resolved(total);
}
