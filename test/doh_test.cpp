// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// DNS over HTTPS (RFC 8484), against anyhttp's server with a certificate made up for the
// occasion. Only built where the tests are on c_ares_async::anyhttp, on either runtime.
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

#include <boost/asio/ip/tcp.hpp>

#include <unistd.h>

using c_ares_async::error_code;
using c_ares_async::log_level;
using c_ares_async::tls_options;
using testing::AllOf;
using testing::Each;
using testing::ElementsAre;
using testing::Field;
using testing::IsEmpty;
using testing::Not;

using transport = fake_dns_server::transport;
using https_request = fake_dns_server::https_request;

namespace
{

constexpr const char* server_name = "dns.example.test";

tls_options https(std::string hostname, bool verify, std::string path = "/dns-query")
{
   return {.enabled = true,
           .https = true,
           .hostname = std::move(hostname),
           .verify = verify,
           .path = std::move(path)};
}

} // namespace

class DnsOverHttps : public event_loop_fixture
{
protected:
   void SetUp() override
   {
      //
      // On the resolver's executor, which is anyhttp's runtime, as for DNS over QUIC.
      //
      server.use_https(executor(), certificate, server_path(), max_streams());
      server.add_address("www.example.test", "192.0.2.1");
      server.add_https("www.example.test", 1, ".", {{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}}});
      server.add_address("mail.example.test", "192.0.2.2");
   }

   void TearDown() override
   {
      //
      // The resolver, the server, and then whatever they leave on the context, until there is
      // nothing left: sessions that are ending, mostly, see DnsOverQuic.
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

   virtual std::string server_path() const { return "/dns-query"; }
   virtual std::uint64_t max_streams() const { return 100; }

   //
   // As for DNS over TLS: the certificate is trusted by way of SSL_CERT_FILE, which anyhttp's
   // client goes by just the same, as it verifies against the default trust store.
   //
   void trust_the_server()
   {
      trusted = testing::TempDir() + "c_ares_async_doh_test_" + std::to_string(::getpid()) + ".pem";
      std::ofstream(trusted) << certificate.certificate_pem;
      ASSERT_EQ(::setenv("SSL_CERT_FILE", trusted.c_str(), 1), 0);
   }

   void make_resolver(const tls_options& tls)
   {
      resolver = std::make_unique<c_ares_async::resolver>(executor(), tls);
      resolver->set_logger([this](log_level level, std::string_view message) {
         messages.emplace_back(level, message);
      });

      ASSERT_FALSE(resolver->set_servers(server.https_address()));
   }

   auto resolve(std::string_view host) { return run(resolver->resolve(std::string(host), "443")); }

   std::size_t logged_count(log_level level, std::string_view text) const
   {
      return std::ranges::count_if(messages, [&](const auto& message) {
         return message.first == level && message.second.find(text) != std::string::npos;
      });
   }

   bool logged(log_level level, std::string_view text) const
   {
      return logged_count(level, text) > 0;
   }

   static inline const test_certificate certificate = make_test_certificate(server_name);

   fake_dns_server server{server_executor()};
   std::unique_ptr<c_ares_async::resolver> resolver;
   std::vector<std::pair<log_level, std::string>> messages;
   std::string trusted;
};

// --------------------------------------------------------------------------------------------------

TEST_F(DnsOverHttps, ResolvesOverHttpsAndNothingElse)
{
   make_resolver(https({}, false));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));

   EXPECT_GE(server.count(transport::https), 1u);
   EXPECT_EQ(server.count(transport::udp), 0u);
   EXPECT_EQ(server.count(transport::tcp), 0u);
   EXPECT_EQ(server.count(transport::tls), 0u);
   EXPECT_EQ(server.count(transport::quic), 0u);
   EXPECT_TRUE(logged(log_level::info, "over HTTP/2"));
}

TEST_F(DnsOverHttps, PostsEveryQueryAsADnsMessageWithAnIdOfZero)
{
   make_resolver(https({}, false));

   const auto [ec, endpoints] = resolve("www.example.test");
   ASSERT_FALSE(ec) << ec.message();

   //
   // RFC 8484, section 4.1. c-ares picks an ID of its own for every query, and tells the answers
   // apart by it -- which it still can, as it gets its own IDs back.
   //
   EXPECT_THAT(server.https_requests(), Not(IsEmpty()));
   EXPECT_THAT(
      server.https_requests(),
      Each(AllOf(Field(&https_request::method, "POST"), Field(&https_request::path, "/dns-query"),
                 Field(&https_request::content_type, "application/dns-message"),
                 Field(&https_request::accept, "application/dns-message"),
                 Field(&https_request::id, 0u))));
}

TEST_F(DnsOverHttps, VerifiesTheCertificateAgainstTheName)
{
   trust_the_server();
   make_resolver(https(server_name, true));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_TRUE(logged(log_level::info, std::string("verified for '") + server_name + "'"));
}

TEST_F(DnsOverHttps, RejectsACertificateForAnotherName)
{
   trust_the_server();
   make_resolver(https("other.example.test", true));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_THAT(server.https_requests(), IsEmpty())
      << "not a question before the server is who it says";
   EXPECT_TRUE(logged(log_level::error, "DoH connection to 127.0.0.1:"));
}

TEST_F(DnsOverHttps, RejectsACertificateNobodyVouchesFor)
{
   make_resolver(https(server_name, true));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_THAT(server.https_requests(), IsEmpty());
}

TEST_F(DnsOverHttps, TakesAnyCertificateWhenToldNotToVerify)
{
   make_resolver(https("other.example.test", false));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_TRUE(logged(log_level::info, "NOT (verify is off)"));
}

//
// Unlike DNS over TLS and DNS over QUIC, which check the chain only where there is no name: the
// URL has a host, which is the server's address then, and the certificate has to be for that.
// The one here is for the name only.
//
TEST_F(DnsOverHttps, VerifiesTheCertificateAgainstTheAddressWhereThereIsNoName)
{
   trust_the_server();
   make_resolver(https({}, true));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_THAT(server.https_requests(), IsEmpty());
}

TEST_F(DnsOverHttps, KeepsTheConnectionForTheNextQuestion)
{
   make_resolver(https({}, false));

   for (const auto* host : {"www.example.test", "mail.example.test", "www.example.test"})
   {
      const auto [ec, endpoints] = resolve(host);
      EXPECT_FALSE(ec) << ec.message();
   }

   EXPECT_EQ(logged_count(log_level::info, "established with"), 1u)
      << "a handshake per name is what this is there to avoid";
}

TEST_F(DnsOverHttps, SendsAddressAndHttpsQueriesDownOneConnection)
{
   make_resolver(https({}, false));

   std::optional<std::tuple<error_code, c_ares_async::resolver::results_type>> addresses;
   std::optional<std::tuple<error_code, c_ares_async::resolver::https_results_type>> records;

   start([](c_ares_async::resolver& resolver, auto& addresses) -> c_ares_async::Task<void> {
      addresses = co_await resolver.resolve("www.example.test", "443");
   }(*resolver, addresses));

   start([](c_ares_async::resolver& resolver, auto& records) -> c_ares_async::Task<void> {
      records = co_await resolver.resolve_https("www.example.test");
   }(*resolver, records));

   ASSERT_TRUE(run_until([&] { return addresses && records; }));

   const auto& [ec, endpoints] = *addresses;
   const auto& [https_ec, found] = *records;

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_FALSE(https_ec) << https_ec.message();
   ASSERT_EQ(found.size(), 1u);
   EXPECT_THAT(found[0].params, ElementsAre("alpn=h3"));

   EXPECT_EQ(logged_count(log_level::info, "established with"), 1u);
   EXPECT_GE(server.https_requests().size(), 3u) << "A, AAAA and HTTPS, each a request of its own";
}

TEST_F(DnsOverHttps, ReportsAServerThatIsNotThere)
{
   make_resolver(https({}, false));

   //
   // A port that was free a moment ago, with nobody listening on it now, which TCP is told at once.
   //
   std::string nowhere;
   {
      fake_dns_server gone(server_executor());
      nowhere = gone.address();
   }
   ASSERT_FALSE(resolver->set_servers(nowhere));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_EQ(ec, c_ares_async::make_ares_error(ARES_ECONNREFUSED)) << ec.message();
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_TRUE(logged(log_level::error, "DoH connection to 127.0.0.1:"));
}

//
// The server is not told where a request comes from, so this is as far as the tests see: a local
// address the host has works, and one that it does not have fails the connection. anyhttp's own
// tests see the TCP connection come from the address.
//
TEST_F(DnsOverHttps, SendsFromTheLocalAddress)
{
   make_resolver(https({}, false));
   resolver->set_local_address(ip_address("127.0.0.3"));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_THAT(server.https_requests(), Not(IsEmpty()));
}

TEST_F(DnsOverHttps, KeepsTheLocalAddressToItsFamily)
{
   make_resolver(https({}, false));
   resolver->set_local_address(ip_address("2001:db8::1"));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(server.https_requests(), Not(IsEmpty()));
}

TEST_F(DnsOverHttps, FailsWhereTheLocalAddressIsNotOneOfTheHosts)
{
   make_resolver(https({}, false));
   resolver->set_local_address(ip_address("192.0.2.9"));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_EQ(ec, c_ares_async::make_ares_error(ARES_ECONNREFUSED)) << ec.message();
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_THAT(server.https_requests(), IsEmpty());
   EXPECT_TRUE(logged(log_level::error, "connection to 127.0.0.1:"));
}

//
// A status that is not 2xx, and an answer that is no DNS message, are the end of the connection
// for c-ares, which fails the server then -- rather than waiting for every query to time out.
//
TEST_F(DnsOverHttps, GivesUpOnAServerThatAnswersWithAnError)
{
   server.set_https_status(503);
   make_resolver(https({}, false));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_TRUE(logged(log_level::error, "the server answers a query with status 503"));
   EXPECT_FALSE(logged(log_level::debug, "query timeout"));
}

TEST_F(DnsOverHttps, GivesUpOnAnAnswerThatIsNoDnsMessage)
{
   server.set_https_content_type("text/html");
   make_resolver(https({}, false));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_TRUE(logged(log_level::error, "a body of type 'text/html'"));
}

TEST_F(DnsOverHttps, TakesTheTypeOfAnAnswerWhateverItsCase)
{
   server.set_https_content_type("Application/DNS-Message; charset=binary");
   make_resolver(https({}, false));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

//
// The server has gone away and come back on the same port, and c-ares learns about the session
// that ended with it from the next query, which the session cannot take: it reconnects, and asks
// again.
//
TEST_F(DnsOverHttps, ConnectsAgainOnceTheServerHasGoneAway)
{
   make_resolver(https({}, false));

   const auto [ec, endpoints] = resolve("www.example.test");
   ASSERT_FALSE(ec) << ec.message();

   server.restart_https();
   std::ignore = run_until([] { return false; }, std::chrono::milliseconds(50));

   //
   // Another name, as c-ares has the answer for the first one in its cache.
   //
   const auto [again, more] = resolve("mail.example.test");
   EXPECT_FALSE(again) << again.message();
   EXPECT_THAT(more, ElementsAre(endpoint("192.0.2.2", 443)));
   EXPECT_EQ(logged_count(log_level::info, "established with"), 2u);
   EXPECT_TRUE(logged(log_level::debug, "connection over"));
}

TEST_F(DnsOverHttps, LetsGoOfAQueryInFlightWhenDestroyed)
{
   server.set_https_hold(true);
   make_resolver(https({}, false));

   std::optional<std::tuple<error_code, c_ares_async::resolver::results_type>> result;
   start([](c_ares_async::resolver& resolver, auto& result) -> c_ares_async::Task<void> {
      result = co_await resolver.resolve("www.example.test", "443");
   }(*resolver, result));

   ASSERT_TRUE(run_until([&] { return !server.https_requests().empty(); }));
   resolver.reset();

   //
   // The bridge has requests out, and the session, and goes on after the resolver: it must not
   // log to the resolver's logger, or touch anything else of it, once that is gone.
   //
   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_EQ(std::get<0>(*result), c_ares_async::errors::canceled);
   std::ignore = run_until([] { return false; }, std::chrono::milliseconds(100));
}

TEST_F(DnsOverHttps, LetsGoOfAConnectionBeingMadeWhenDestroyed)
{
   make_resolver(https({}, false));

   //
   // A listener that never accepts: the kernel completes the TCP handshake, and the TLS handshake
   // is still waiting for an answer when the resolver goes.
   //
   boost::asio::ip::tcp::acceptor black_hole(server_executor(),
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

//
// A stream that the server resets, before there is a response, has its query sent again -- once.
// That is for a server that refuses a stream over the limit it has not told the session about
// yet, see DnsOverHttpsOneStream; a server that resets them all does not get asked for ever.
//
TEST_F(DnsOverHttps, AsksAgainOnceWhereTheServerResetsTheStream)
{
   server.set_silent(true); // which resets the stream, with no response on it

   ASSERT_EQ(::setenv("RES_OPTIONS", "timeout:1 attempts:1", 1), 0);
   make_resolver(https({}, false));
   ASSERT_EQ(::unsetenv("RES_OPTIONS"), 0);

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_EQ(ec, c_ares_async::make_ares_error(ARES_ETIMEOUT)) << ec.message();
   EXPECT_THAT(endpoints, IsEmpty());
   ASSERT_THAT(server.queries(), Not(IsEmpty()));
   for (const auto& query : server.queries())
      EXPECT_EQ(server.count(query.name, query.type), 2u) << query.name << " " << query.type;
   EXPECT_TRUE(logged(log_level::debug, "sending the query again"));
}

TEST_F(DnsOverHttps, CannotBeAskedForQuicAsWell)
{
   auto options = https({}, false);
   options.quic = true;

   try
   {
      c_ares_async::resolver both(executor(), options);
      FAIL() << "a resolver for DNS over QUIC and DNS over HTTPS at once";
   }
   catch (const c_ares_async::system_error& error)
   {
      EXPECT_EQ(error.code(), c_ares_async::errc::invalid_argument) << error.what();
   }
}

// --------------------------------------------------------------------------------------------------

class DnsOverHttpsElsewhere : public DnsOverHttps
{
protected:
   std::string server_path() const override { return "/resolve"; }
};

TEST_F(DnsOverHttpsElsewhere, AsksAtThePathItIsGiven)
{
   make_resolver(https({}, false, "/resolve"));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_THAT(server.https_requests(), Each(Field(&https_request::path, "/resolve")));
}

TEST_F(DnsOverHttpsElsewhere, GivesUpOnAServerThatHasNothingAtThePath)
{
   make_resolver(https({}, false));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_TRUE(logged(log_level::error, "the server answers a query with status 404"));
}

// --------------------------------------------------------------------------------------------------

class DnsOverHttpsOneStream : public DnsOverHttps
{
protected:
   std::uint64_t max_streams() const override { return 1; }
};

TEST_F(DnsOverHttpsOneStream, SendsEachQueryOnceTheServerAllowsAnotherStream)
{
   make_resolver(https({}, false));

   std::optional<std::tuple<error_code, c_ares_async::resolver::results_type>> addresses;
   std::optional<std::tuple<error_code, c_ares_async::resolver::https_results_type>> records;

   start([](c_ares_async::resolver& resolver, auto& addresses) -> c_ares_async::Task<void> {
      addresses = co_await resolver.resolve("www.example.test", "443");
   }(*resolver, addresses));

   start([](c_ares_async::resolver& resolver, auto& records) -> c_ares_async::Task<void> {
      records = co_await resolver.resolve_https("www.example.test");
   }(*resolver, records));

   ASSERT_TRUE(run_until([&] { return addresses && records; }));

   const auto& [ec, endpoints] = *addresses;
   const auto& [https_ec, found] = *records;

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_FALSE(https_ec) << https_ec.message();
   EXPECT_EQ(found.size(), 1u);

   //
   // One request at a time, and the next once the answer to the one before is in -- not once
   // c-ares has given up on the query and sent it again. Until the server's SETTINGS are in, the
   // session does not know about the limit, and the server refuses the streams over it, which
   // never get to the server's handler: those queries go out again, see AsksAgainOnce... -- on
   // Corosio, mostly, which sends before the SETTINGS are in.
   //
   EXPECT_EQ(logged_count(log_level::info, "established with"), 1u);
   EXPECT_EQ(server.https_requests().size(), 3u) << "A, AAAA and HTTPS, and each of them once";
   EXPECT_TRUE(logged(log_level::debug, "1 queries out, as many as the server takes"));
   EXPECT_FALSE(logged(log_level::debug, "query timeout"));
}
