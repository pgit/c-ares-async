// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#include "fake_dns_server.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#if C_ARES_ASYNC_DOQ
#include <anyhttp/quic.hpp>
#include <anyhttp/server.hpp>

#include <spdlog/spdlog.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>

using namespace boost::asio;
using boost::system::error_code;

namespace
{

constexpr auto awaited = as_tuple(use_awaitable);

//
// Names are compared the way DNS compares them: whatever the case, with or without the dot at
// the end.
//
std::string normalized(std::string_view name)
{
   if (name.ends_with('.'))
      name.remove_suffix(1);

   std::string result(name);
   std::ranges::transform(result, result.begin(), [](unsigned char c) { return std::tolower(c); });
   return result;
}

void check(ares_status_t status, const char* what)
{
   if (status != ARES_SUCCESS)
      throw std::runtime_error(std::string(what) + ": " + ares_strerror(static_cast<int>(status)));
}

struct https_data
{
   unsigned short priority;
   std::string target;
   std::vector<fake_dns_server::svc_param> params;
};

struct records
{
   std::vector<ip::address> addresses;
   std::optional<std::string> cname;
   std::vector<https_data> https;
};

using record_ptr = std::unique_ptr<ares_dns_record_t, decltype(&ares_dns_record_destroy)>;

} // namespace

// --------------------------------------------------------------------------------------------------

struct fake_dns_server::state
{
   explicit state(const any_io_executor& executor) : udp(executor), acceptor(executor) {}

   std::vector<unsigned char> answer(std::span<const unsigned char> message, transport via);
   void add_answers(ares_dns_record_t* response, std::string owner, ares_dns_rec_type_t type) const;

   //
   // The serving is done by coroutines, each of which keeps the state alive for as long as it
   // runs -- that may be longer than the server itself.
   //
   static awaitable<void> serve_udp(std::shared_ptr<state> self);
   static awaitable<void> serve_tcp(std::shared_ptr<state> self);
   static awaitable<void> serve_connection(std::shared_ptr<state> self, ip::tcp::socket socket);

   template <typename Stream>
   static awaitable<void> serve_stream(std::shared_ptr<state> self, Stream& stream, transport via);

#if C_ARES_ASYNC_DOQ
   static awaitable<void> serve_quic(std::shared_ptr<state> self,
                                     anyhttp::quic::Connection connection);
   static awaitable<void> serve_quic_stream(std::shared_ptr<state> self,
                                            anyhttp::quic::Stream stream);

   std::optional<anyhttp::server::Server> quic;
   bool quic_extra_message = false;
   std::size_t quic_connections = 0;
   std::vector<unsigned> quic_ids;
#endif

   ip::udp::socket udp;
   ip::tcp::acceptor acceptor;
   std::optional<ssl::context> tls;

   std::map<std::string, records, std::less<>> zone;
   bool silent = false;
   bool truncate_udp = false;
   std::optional<ares_dns_rcode_t> rcode;

   std::vector<query> queries;
   std::size_t connections = 0;
   std::size_t handshakes = 0;
};

//
// The answer section for 'owner' and 'type': its records of that type, behind whatever chain of
// CNAMEs leads to them.
//
void fake_dns_server::state::add_answers(ares_dns_record_t* response, std::string owner,
                                         ares_dns_rec_type_t type) const
{
   const auto add = [&](ares_dns_rec_type_t rr_type) {
      ares_dns_rr_t* rr = nullptr;
      check(ares_dns_record_rr_add(&rr, response, ARES_SECTION_ANSWER, owner.c_str(), rr_type,
                                   ARES_CLASS_IN, 60),
            "ares_dns_record_rr_add");
      return rr;
   };

   for (int hops = 0; hops < 8; ++hops)
   {
      const auto it = zone.find(owner);
      if (it == zone.end())
         return;

      const auto& found = it->second;
      if (found.cname && type != ARES_REC_TYPE_CNAME)
      {
         check(ares_dns_rr_set_str(add(ARES_REC_TYPE_CNAME), ARES_RR_CNAME_CNAME,
                                   found.cname->c_str()),
               "ares_dns_rr_set_str");
         owner = *found.cname;
         continue;
      }

      for (const auto& address : found.addresses)
      {
         if (type == ARES_REC_TYPE_A && address.is_v4())
         {
            in_addr addr{};
            std::memcpy(&addr, address.to_v4().to_bytes().data(), sizeof(addr));
            check(ares_dns_rr_set_addr(add(type), ARES_RR_A_ADDR, &addr), "ares_dns_rr_set_addr");
         }
         else if (type == ARES_REC_TYPE_AAAA && address.is_v6())
         {
            ares_in6_addr addr{};
            std::memcpy(&addr, address.to_v6().to_bytes().data(), sizeof(addr));
            check(ares_dns_rr_set_addr6(add(type), ARES_RR_AAAA_ADDR, &addr),
                  "ares_dns_rr_set_addr6");
         }
      }

      if (type == ARES_REC_TYPE_HTTPS)
      {
         for (const auto& https : found.https)
         {
            auto* rr = add(type);
            check(ares_dns_rr_set_u16(rr, ARES_RR_HTTPS_PRIORITY, https.priority),
                  "ares_dns_rr_set_u16");
            check(ares_dns_rr_set_str(rr, ARES_RR_HTTPS_TARGET, https.target.c_str()),
                  "ares_dns_rr_set_str");
            for (const auto& param : https.params)
               check(ares_dns_rr_set_opt(rr, ARES_RR_HTTPS_PARAMS, param.key, param.value.data(),
                                         param.value.size()),
                     "ares_dns_rr_set_opt");
         }
      }

      return;
   }
}

//
// The reply to one message, or nothing at all where there is none to give.
//
std::vector<unsigned char> fake_dns_server::state::answer(std::span<const unsigned char> message,
                                                          transport via)
{
   ares_dns_record_t* parsed = nullptr;
   if (ares_dns_parse(message.data(), message.size(), 0, &parsed) != ARES_SUCCESS)
      return {};

   const record_ptr request(parsed, &ares_dns_record_destroy);

   const char* name = nullptr;
   ares_dns_rec_type_t type{};
   ares_dns_class_t qclass{};
   if (ares_dns_record_query_get(request.get(), 0, &name, &type, &qclass) != ARES_SUCCESS)
      return {};

   queries.push_back({normalized(name), type, via});
   if (silent)
      return {};

   ares_dns_record_t* created = nullptr;
   unsigned short flags = ARES_FLAG_QR | ARES_FLAG_RD | ARES_FLAG_RA;
   const bool truncated = truncate_udp && via == transport::udp;
   if (truncated)
      flags |= ARES_FLAG_TC;

   //
   // A name with no record of the type asked for is an answer with nothing in it. One that is
   // not there at all is an error.
   //
   const bool exists = zone.contains(normalized(name));
   const auto code = rcode.value_or(exists ? ARES_RCODE_NOERROR : ARES_RCODE_NXDOMAIN);
   check(ares_dns_record_create(&created, ares_dns_record_get_id(request.get()), flags,
                                ARES_OPCODE_QUERY, code),
         "ares_dns_record_create");

   const record_ptr response(created, &ares_dns_record_destroy);
   check(ares_dns_record_query_add(response.get(), name, type, qclass),
         "ares_dns_record_query_add");

   if (!rcode && !truncated)
      add_answers(response.get(), normalized(name), type);

   unsigned char* bytes = nullptr;
   std::size_t length = 0;
   check(ares_dns_write(response.get(), &bytes, &length), "ares_dns_write");

   std::vector<unsigned char> reply(bytes, bytes + length);
   ares_free_string(bytes);
   return reply;
}

// --------------------------------------------------------------------------------------------------

fake_dns_server::fake_dns_server(const any_io_executor& executor)
   : m_state(std::make_shared<state>(executor))
{
   //
   // Any port will do, but it has to be free for UDP and TCP both -- which the one the system
   // picks for UDP is, more often than not.
   //
   for (int attempt = 0;; ++attempt)
   {
      m_state->udp = ip::udp::socket(executor, {ip::address_v4::loopback(), 0});

      error_code ec;
      m_state->acceptor = ip::tcp::acceptor(executor);
      m_state->acceptor.open(ip::tcp::v4());
      m_state->acceptor.set_option(ip::tcp::acceptor::reuse_address(true));
      m_state->acceptor.bind({ip::address_v4::loopback(), m_state->udp.local_endpoint().port()},
                             ec);
      if (!ec)
         break;

      if (attempt == 16)
         throw boost::system::system_error(ec, "no port free for both UDP and TCP");
   }

   m_state->acceptor.listen();

   co_spawn(executor, state::serve_udp(m_state), detached);
   co_spawn(executor, state::serve_tcp(m_state), detached);
}

fake_dns_server::~fake_dns_server()
{
   error_code ignored;
   m_state->udp.close(ignored);
   m_state->acceptor.close(ignored);
#if C_ARES_ASYNC_DOQ
   m_state->quic.reset(); // stops listening, and ends every connection
#endif
}

#if C_ARES_ASYNC_DOQ
void fake_dns_server::shutdown()
{
   error_code ignored;
   m_state->udp.close(ignored);
   m_state->acceptor.close(ignored);
   m_state->quic.reset();
}
#endif

std::string fake_dns_server::address() const
{
   return "127.0.0.1:" + std::to_string(m_state->udp.local_endpoint().port());
}

void fake_dns_server::add_address(std::string_view name, std::string_view address)
{
   m_state->zone[normalized(name)].addresses.push_back(ip::make_address(address));
}

void fake_dns_server::add_cname(std::string_view name, std::string_view target)
{
   m_state->zone[normalized(name)].cname = normalized(target);
}

void fake_dns_server::add_https(std::string_view name, unsigned short priority,
                                std::string_view target, std::vector<svc_param> params)
{
   m_state->zone[normalized(name)].https.push_back(
      {priority, std::string(target), std::move(params)});
}

void fake_dns_server::set_silent(bool silent) { m_state->silent = silent; }

void fake_dns_server::set_rcode(ares_dns_rcode_t rcode) { m_state->rcode = rcode; }

void fake_dns_server::set_truncate_udp(bool truncate) { m_state->truncate_udp = truncate; }

void fake_dns_server::use_tls(const test_certificate& certificate)
{
   auto& tls = m_state->tls.emplace(ssl::context::tls_server);
   tls.use_certificate_chain(buffer(certificate.certificate_pem));
   tls.use_private_key(buffer(certificate.key_pem), ssl::context::pem);
}

const std::vector<fake_dns_server::query>& fake_dns_server::queries() const
{
   return m_state->queries;
}

std::size_t fake_dns_server::count(std::string_view name, ares_dns_rec_type_t type) const
{
   return std::ranges::count_if(m_state->queries, [&, name = normalized(name)](const query& q) {
      return q.name == name && q.type == type;
   });
}

std::size_t fake_dns_server::count(transport via) const
{
   return std::ranges::count(m_state->queries, via, &query::via);
}

std::size_t fake_dns_server::connections() const { return m_state->connections; }

std::size_t fake_dns_server::handshakes() const { return m_state->handshakes; }

// --------------------------------------------------------------------------------------------------

awaitable<void> fake_dns_server::state::serve_udp(std::shared_ptr<state> self)
{
   std::array<unsigned char, 4096> message;
   for (;;)
   {
      ip::udp::endpoint peer;
      const auto [ec, size] = co_await self->udp.async_receive_from(buffer(message), peer, awaited);
      if (ec)
         co_return; // closed, which is how the server goes away

      const auto reply = self->answer({message.data(), size}, transport::udp);
      if (!reply.empty())
         co_await self->udp.async_send_to(buffer(reply), peer, awaited);
   }
}

//
// DNS over a stream is the same messages, each behind two octets that say how long it is. One
// connection carries as many of them as the resolver cares to send.
//
template <typename Stream>
awaitable<void> fake_dns_server::state::serve_stream(std::shared_ptr<state> self, Stream& stream,
                                                     transport via)
{
   for (;;)
   {
      std::array<unsigned char, 2> prefix;
      if (const auto [ec, size] = co_await async_read(stream, buffer(prefix), awaited); ec)
         co_return;

      std::vector<unsigned char> message((prefix[0] << 8) | prefix[1]);
      if (const auto [ec, size] = co_await async_read(stream, buffer(message), awaited); ec)
         co_return;

      const auto reply = self->answer(message, via);
      if (reply.empty())
         continue;

      prefix = {static_cast<unsigned char>(reply.size() >> 8),
                static_cast<unsigned char>(reply.size() & 0xff)};
      const std::array<const_buffer, 2> buffers{buffer(prefix), buffer(reply)};
      if (const auto [ec, size] = co_await async_write(stream, buffers, awaited); ec)
         co_return;
   }
}

awaitable<void> fake_dns_server::state::serve_connection(std::shared_ptr<state> self,
                                                         ip::tcp::socket socket)
{
   if (!self->tls)
   {
      co_await serve_stream(self, socket, transport::tcp);
      co_return;
   }

   ssl::stream<ip::tcp::socket> stream(std::move(socket), *self->tls);
   if (const auto [ec] = co_await stream.async_handshake(ssl::stream_base::server, awaited); ec)
      co_return; // the client did not like the certificate, most likely

   ++self->handshakes;
   co_await serve_stream(self, stream, transport::tls);
}

awaitable<void> fake_dns_server::state::serve_tcp(std::shared_ptr<state> self)
{
   for (;;)
   {
      auto [ec, socket] = co_await self->acceptor.async_accept(awaited);
      if (ec)
         co_return;

      ++self->connections;

      //
      // The length and the message behind it may go out as two segments, and the second would
      // wait for the first to be acknowledged -- which the other side is in no hurry to do.
      //
      error_code ignored;
      socket.set_option(ip::tcp::no_delay(true), ignored);
      co_spawn(self->acceptor.get_executor(), serve_connection(self, std::move(socket)), detached);
   }
}

// --------------------------------------------------------------------------------------------------

#if C_ARES_ASYNC_DOQ

void fake_dns_server::use_quic(const test_certificate& certificate,
                               std::chrono::nanoseconds idle_timeout)
{
   //
   // anyhttp has the defaults of a development server, and says so at the level of information:
   // what it has to say about the client going wrong is a warning, and that is all the tests want
   // to see of it.
   //
   spdlog::set_level(spdlog::level::warn);

   //
   // anyhttp reads its certificate from files, once, when the server is made.
   //
   const auto base = testing::TempDir() + "c_ares_async_doq_" + std::to_string(::getpid());
   const auto chain = base + "_chain.pem", key = base + "_key.pem";
   std::ofstream(chain) << certificate.certificate_pem;
   std::ofstream(key) << certificate.key_pem;

   m_state->quic.emplace(m_state->udp.get_executor(), anyhttp::server::Config{.listen_address = "127.0.0.1",
                                                                 .port = 0,
                                                                 .tls_certificate_chain = chain,
                                                                 .tls_private_key = key,
                                                                 .idle_timeout = idle_timeout});
   std::remove(chain.c_str());
   std::remove(key.c_str());

   //
   // The server keeps the handler, so the handler must not keep the state, which has the server.
   //
   m_state->quic->on_quic(
      {.alpn = "doq"},
      [weak = std::weak_ptr<state>(m_state)](anyhttp::quic::Connection connection) -> awaitable<void> {
         if (auto self = weak.lock())
            co_await state::serve_quic(std::move(self), std::move(connection));
      });
}

std::string fake_dns_server::quic_address() const
{
   return "127.0.0.1:" + std::to_string(m_state->quic->local_endpoint().port());
}

void fake_dns_server::set_quic_extra_message(bool extra) { m_state->quic_extra_message = extra; }

std::size_t fake_dns_server::quic_connections() const { return m_state->quic_connections; }

const std::vector<unsigned>& fake_dns_server::quic_ids() const { return m_state->quic_ids; }

//
// RFC 9250: the client opens a stream for every query, and the server does nothing but answer on
// it. The connection is over when the handler returns, so it waits for the client to end it.
//
awaitable<void> fake_dns_server::state::serve_quic(std::shared_ptr<state> self,
                                                   anyhttp::quic::Connection connection)
{
   ++self->quic_connections;
   for (;;)
   {
      auto [ec, stream] = co_await connection.accept_stream();
      if (ec)
         co_return;

      co_spawn(connection.get_executor(), serve_quic_stream(self, std::move(stream)), detached);
   }
}

awaitable<void> fake_dns_server::state::serve_quic_stream(std::shared_ptr<state> self,
                                                          anyhttp::quic::Stream stream)
{
   std::vector<unsigned char> query;
   std::array<unsigned char, 4096> buffer;
   for (;;)
   {
      auto [ec, n] = co_await stream.read_some(boost::asio::buffer(buffer));
      query.insert(query.end(), buffer.begin(), buffer.begin() + n);
      if (ec == anyhttp::errors::eof)
         break;
      if (ec)
         co_return;
   }

   if (query.size() < 2 + 12 || ((std::size_t{query[0]} << 8) | query[1]) != query.size() - 2)
      co_return; // which resets the stream

   self->quic_ids.push_back((unsigned{query[2]} << 8) | query[3]);

   auto reply = self->answer({query.data() + 2, query.size() - 2}, transport::quic);
   if (reply.empty())
      co_return; // silent: no answer, and the stream reset

   std::vector<unsigned char> framed{static_cast<unsigned char>(reply.size() >> 8),
                                     static_cast<unsigned char>(reply.size() & 0xff)};
   framed.insert(framed.end(), reply.begin(), reply.end());
   if (self->quic_extra_message)
   {
      const auto copy = framed;
      framed.insert(framed.end(), copy.begin(), copy.end());
   }

   std::ignore = co_await stream.write_eof(boost::asio::buffer(framed));
}

#endif // C_ARES_ASYNC_DOQ
