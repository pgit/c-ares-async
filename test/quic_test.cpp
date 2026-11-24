// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// DNS over QUIC (RFC 9250), against anyhttp's server with a certificate made up for the occasion.
// Only built where the tests are on c_ares_async::doq, on either runtime.
//
#include "resolver_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/ip/udp.hpp>

#include <unistd.h>

using c_ares_async::error_code;
using c_ares_async::log_level;
using c_ares_async::tls_options;
using testing::Contains;
using testing::Each;
using testing::ElementsAre;
using testing::Field;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;
using testing::Pair;

using transport = fake_dns_server::transport;

namespace
{

constexpr const char* server_name = "dns.example.test";

tls_options quic(std::string hostname, bool verify)
{
   return {.enabled = true, .quic = true, .hostname = std::move(hostname), .verify = verify};
}

} // namespace

class DnsOverQuic : public event_loop_fixture
{
protected:
   void SetUp() override
   {
      //
      // On the resolver's executor, which is anyhttp's runtime: the server's own is Asio's on
      // Corosio as well, and the same context on Asio.
      //
      server.use_quic(executor(), certificate, idle_timeout(), max_streams());
      server.add_address("www.example.test", "192.0.2.1");
      server.add_https("www.example.test", 1, ".", {{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}}});
      server.add_address("mail.example.test", "192.0.2.2");
   }

   void TearDown() override
   {
      //
      // The resolver, the server, and then whatever they leave on the context, until there is
      // nothing left: QUIC connections that are closing, mostly. Destroyed with that pending, the
      // context would not free what anyhttp holds on to until it is through. That is the
      // resolver's context, on either runtime, which is where anyhttp's side of the server is.
      //
      resolver.reset();
      server.shutdown();
      context.restart();
      context.run_for(std::chrono::seconds(10));
      EXPECT_TRUE(context.stopped()) << "still running after 10s";

      ::unsetenv("SSL_CERT_FILE");
      if (!trusted.empty())
         std::remove(trusted.c_str());
   }

   virtual std::chrono::nanoseconds idle_timeout() const { return std::chrono::seconds(30); }
   virtual std::uint64_t max_streams() const { return 100; }

   //
   // As for DNS over TLS: the certificate is trusted by way of SSL_CERT_FILE, which anyhttp's
   // client goes by just the same, as it verifies against the default trust store.
   //
   void trust_the_server()
   {
      trusted = testing::TempDir() + "c_ares_async_doq_test_" + std::to_string(::getpid()) + ".pem";
      std::ofstream(trusted) << certificate.certificate_pem;
      ASSERT_EQ(::setenv("SSL_CERT_FILE", trusted.c_str(), 1), 0);
   }

   void make_resolver(const tls_options& tls)
   {
      resolver = std::make_unique<c_ares_async::resolver>(executor(), tls);
      resolver->set_logger([this](log_level level, std::string_view message) {
         messages.emplace_back(level, message);
      });

      ASSERT_FALSE(resolver->set_servers(server.quic_address()));
   }

   auto resolve(std::string_view host) { return run(resolver->resolve(std::string(host), "443")); }

   bool logged(log_level level, std::string_view text) const
   {
      return std::ranges::any_of(messages, [&](const auto& message) {
         return message.first == level && message.second.find(text) != std::string::npos;
      });
   }

   static inline const test_certificate certificate = make_test_certificate(server_name);

   fake_dns_server server{server_executor()};
   std::unique_ptr<c_ares_async::resolver> resolver;
   std::vector<std::pair<log_level, std::string>> messages;
   std::string trusted;
};

// --------------------------------------------------------------------------------------------------

TEST_F(DnsOverQuic, ResolvesOverQuicAndNothingElse)
{
   make_resolver(quic({}, false));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));

   EXPECT_EQ(server.quic_connections(), 1u);
   EXPECT_GE(server.count(transport::quic), 1u);
   EXPECT_EQ(server.count(transport::udp), 0u);
   EXPECT_EQ(server.count(transport::tcp), 0u);
   EXPECT_EQ(server.count(transport::tls), 0u);
}

TEST_F(DnsOverQuic, SendsEveryQueryWithAnIdOfZero)
{
   make_resolver(quic({}, false));

   const auto [ec, endpoints] = resolve("www.example.test");
   ASSERT_FALSE(ec) << ec.message();

   //
   // RFC 9250, section 4.2.1. c-ares picks an ID of its own for every query, and tells the
   // answers apart by it -- which it still can, as it gets its own IDs back.
   //
   EXPECT_THAT(server.quic_ids(), Not(IsEmpty()));
   EXPECT_THAT(server.quic_ids(), Each(0u));
}

TEST_F(DnsOverQuic, VerifiesTheCertificateAgainstTheName)
{
   trust_the_server();
   make_resolver(quic(server_name, true));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_EQ(server.quic_connections(), 1u);
}

TEST_F(DnsOverQuic, RejectsACertificateForAnotherName)
{
   trust_the_server();
   make_resolver(quic("other.example.test", true));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_EQ(server.quic_connections(), 0u);
   EXPECT_THAT(server.queries(), IsEmpty()) << "not a question before the server is who it says";
   EXPECT_TRUE(logged(log_level::error, "DoQ connection to 127.0.0.1:"));
}

TEST_F(DnsOverQuic, RejectsACertificateNobodyVouchesFor)
{
   make_resolver(quic(server_name, true));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_THAT(server.queries(), IsEmpty());
}

TEST_F(DnsOverQuic, TakesAnyCertificateWhenToldNotToVerify)
{
   make_resolver(quic("other.example.test", false));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_TRUE(logged(log_level::info, "NOT (verify is off)"));
}

TEST_F(DnsOverQuic, VerifiesTheChainWhereThereIsNoNameToCheck)
{
   trust_the_server();
   make_resolver(quic({}, true));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(DnsOverQuic, KeepsTheConnectionForTheNextQuestion)
{
   make_resolver(quic({}, false));

   for (int i = 0; i < 3; ++i)
   {
      const auto [ec, endpoints] = resolve("www.example.test");
      EXPECT_FALSE(ec) << ec.message();
   }

   EXPECT_EQ(server.quic_connections(), 1u)
      << "a handshake per name is what this is there to avoid";
}

TEST_F(DnsOverQuic, SendsAddressAndHttpsQueriesDownOneConnection)
{
   make_resolver(quic({}, false));

   std::optional<std::tuple<error_code, c_ares_async::resolver::results_type>> addresses;
   std::optional<std::tuple<error_code, c_ares_async::resolver::https_results_type>> https;

   start([](c_ares_async::resolver& resolver, auto& addresses) -> c_ares_async::Task<void> {
      addresses = co_await resolver.resolve("www.example.test", "443");
   }(*resolver, addresses));

   start([](c_ares_async::resolver& resolver, auto& https) -> c_ares_async::Task<void> {
      https = co_await resolver.resolve_https("www.example.test");
   }(*resolver, https));

   ASSERT_TRUE(run_until([&] { return addresses && https; }));

   const auto& [ec, endpoints] = *addresses;
   const auto& [https_ec, records] = *https;

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_FALSE(https_ec) << https_ec.message();
   ASSERT_EQ(records.size(), 1u);
   EXPECT_THAT(records[0].params, ElementsAre("alpn=h3"));

   EXPECT_EQ(server.quic_connections(), 1u);
   EXPECT_GE(server.quic_ids().size(), 3u) << "A, AAAA and HTTPS, each on a stream of its own";
}

TEST_F(DnsOverQuic, ReportsAServerThatIsNotThere)
{
   make_resolver(quic({}, false));

   //
   // A port that was free a moment ago, with nobody listening on it now -- which for QUIC means
   // nobody answering, until the handshake gives up.
   //
   std::string nowhere;
   {
      fake_dns_server gone(server_executor());
      nowhere = gone.address();
   }
   ASSERT_FALSE(resolver->set_servers(nowhere));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
}

//
// c-ares binds a socket of the transport's as it would its own, and the bridge has anyhttp bind
// the UDP socket of the connection there.
//
TEST_F(DnsOverQuic, SendsFromTheLocalAddress)
{
   make_resolver(quic({}, false));
   resolver->set_local_address(ip_address("127.0.0.3"));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_EQ(server.quic_connections(), 1u);
   EXPECT_THAT(server.queries(), Not(IsEmpty()));
   EXPECT_THAT(server.queries(), Each(Field(&fake_dns_server::query::from, "127.0.0.3")));
}

//
// An address the host does not have, which would fail the connection if it were used.
//
TEST_F(DnsOverQuic, KeepsTheLocalAddressToItsFamily)
{
   make_resolver(quic({}, false));
   resolver->set_local_address(ip_address("2001:db8::1"));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(server.queries(), Not(IsEmpty()));
   EXPECT_THAT(server.queries(), Each(Field(&fake_dns_server::query::from, "127.0.0.1")));
}

//
// As for TCP and TLS, though the bind fails once the bridge connects, not in c-ares' abind(): the
// end of the stream that c-ares reads then is what a connection that is refused comes to.
//
TEST_F(DnsOverQuic, FailsWhereTheLocalAddressIsNotOneOfTheHosts)
{
   make_resolver(quic({}, false));
   resolver->set_local_address(ip_address("192.0.2.9"));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_EQ(ec, c_ares_async::make_ares_error(ARES_ECONNREFUSED)) << ec.message();
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_EQ(server.quic_connections(), 0u);
   EXPECT_TRUE(logged(log_level::error, "connection to 127.0.0.1:"));
}

TEST_F(DnsOverQuic, GivesUpOnAServerThatBreaksTheProtocol)
{
   server.set_quic_extra_message(true);
   make_resolver(quic({}, false));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_TRUE(logged(log_level::error, "which is a protocol error"));
}

TEST_F(DnsOverQuic, LetsGoOfAQueryInFlightWhenDestroyed)
{
   server.set_silent(true);
   make_resolver(quic({}, false));

   std::optional<std::tuple<error_code, c_ares_async::resolver::results_type>> result;
   start([](c_ares_async::resolver& resolver, auto& result) -> c_ares_async::Task<void> {
      result = co_await resolver.resolve("www.example.test", "443");
   }(*resolver, result));

   ASSERT_TRUE(run_until([&] { return !server.quic_ids().empty(); }));
   resolver.reset();

   //
   // The bridge has a stream open, and the connection, and goes on after the resolver: it must not
   // log to the resolver's logger, or touch anything else of it, once that is gone.
   //
   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_EQ(std::get<0>(*result), c_ares_async::errors::canceled);
   std::ignore = run_until([] { return false; }, std::chrono::milliseconds(100));
}

TEST_F(DnsOverQuic, LetsGoOfAConnectionBeingMadeWhenDestroyed)
{
   make_resolver(quic({}, false));

   //
   // Nobody answering, so the handshake is still going on when the resolver goes. A port with
   // nobody on it would not do: the loopback interface says so at once, which fails the connection
   // there and then. A socket that is there and never reads takes the datagrams and says nothing.
   //
   boost::asio::ip::udp::socket black_hole(server_executor(),
                                           {boost::asio::ip::address_v4::loopback(), 0});
   ASSERT_FALSE(
      resolver->set_servers("127.0.0.1:" + std::to_string(black_hole.local_endpoint().port())));

   std::optional<std::tuple<error_code, c_ares_async::resolver::results_type>> result;
   start([](c_ares_async::resolver& resolver, auto& result) -> c_ares_async::Task<void> {
      result = co_await resolver.resolve("www.example.test", "443");
   }(*resolver, result));

   std::ignore = run_until([] { return false; }, std::chrono::milliseconds(50));
   resolver.reset();

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_EQ(std::get<0>(*result), c_ares_async::errors::canceled);
   std::ignore = run_until([] { return false; }, std::chrono::milliseconds(100));
}

// --------------------------------------------------------------------------------------------------

class DnsOverQuicIdle : public DnsOverQuic
{
protected:
   std::chrono::nanoseconds idle_timeout() const override { return std::chrono::milliseconds(300); }
};

TEST_F(DnsOverQuicIdle, ConnectsAgainOnceTheConnectionHasTimedOut)
{
   make_resolver(quic({}, false));

   const auto [ec, endpoints] = resolve("www.example.test");
   ASSERT_FALSE(ec) << ec.message();

   //
   // c-ares keeps its connection open (ARES_FLAG_STAYOPEN), and has to learn that the QUIC
   // connection under it is gone -- not from the next query running into it.
   //
   ASSERT_TRUE(run_until([&] { return logged(log_level::debug, "connection over"); }));

   //
   // Another name, as c-ares has the answer for the first one in its cache.
   //
   const auto [again, more] = resolve("mail.example.test");
   EXPECT_FALSE(again) << again.message();
   EXPECT_THAT(more, ElementsAre(endpoint("192.0.2.2", 443)));
   EXPECT_EQ(server.quic_connections(), 2u);
}

// --------------------------------------------------------------------------------------------------

class DnsOverQuicOneStream : public DnsOverQuic
{
protected:
   std::uint64_t max_streams() const override { return 1; }
};

TEST_F(DnsOverQuicOneStream, SendsEachQueryOnceTheServerAllowsAnotherStream)
{
   make_resolver(quic({}, false));

   std::optional<std::tuple<error_code, c_ares_async::resolver::results_type>> addresses;
   std::optional<std::tuple<error_code, c_ares_async::resolver::https_results_type>> https;

   start([](c_ares_async::resolver& resolver, auto& addresses) -> c_ares_async::Task<void> {
      addresses = co_await resolver.resolve("www.example.test", "443");
   }(*resolver, addresses));

   start([](c_ares_async::resolver& resolver, auto& https) -> c_ares_async::Task<void> {
      https = co_await resolver.resolve_https("www.example.test");
   }(*resolver, https));

   ASSERT_TRUE(run_until([&] { return addresses && https; }));

   const auto& [ec, endpoints] = *addresses;
   const auto& [https_ec, records] = *https;

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_FALSE(https_ec) << https_ec.message();
   EXPECT_EQ(records.size(), 1u);

   //
   // One stream at a time, and the next once the server allows it, which is a round trip after
   // the answer on the one before -- not once c-ares has given up on the query and sent it again.
   //
   EXPECT_EQ(server.quic_connections(), 1u);
   EXPECT_EQ(server.quic_ids().size(), 3u) << "A, AAAA and HTTPS, and each of them once";
   EXPECT_FALSE(logged(log_level::debug, "query timeout"));
}

TEST_F(DnsOverQuicOneStream, LetsGoOfAQueryWaitingForAStreamWhenDestroyed)
{
   server.set_quic_hold(true);
   make_resolver(quic({}, false));

   std::optional<std::tuple<error_code, c_ares_async::resolver::results_type>> result;
   start([](c_ares_async::resolver& resolver, auto& result) -> c_ares_async::Task<void> {
      result = co_await resolver.resolve("www.example.test", "443");
   }(*resolver, result));

   //
   // The one stream there is has the first query on it, which is never answered, and the second
   // waits for another stream when the resolver goes.
   //
   ASSERT_TRUE(run_until([&] { return !server.quic_ids().empty(); }));
   std::ignore = run_until([] { return false; }, std::chrono::milliseconds(50));
   EXPECT_EQ(server.quic_ids().size(), 1u);
   resolver.reset();

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_EQ(std::get<0>(*result), c_ares_async::errors::canceled);
   std::ignore = run_until([] { return false; }, std::chrono::milliseconds(100));
}
