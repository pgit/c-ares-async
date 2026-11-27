// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// DNS over QUIC (RFC 9250) on a socket of the anyhttp transport, with anyhttp's QUIC: one QUIC
// connection for the TCP connection c-ares believes it has, and a stream of its own for every
// query. Only where C_ARES_ASYNC_ANYHTTP is 1, as the transport.
//
// https://www.rfc-editor.org/rfc/rfc9250
//
#pragma once

#include <c_ares_async/runtime.hpp>

#if C_ARES_ASYNC_ANYHTTP

#include <c_ares_async/detail/anyhttp_bridge.hpp>

#include <anyhttp/quic.hpp>
#include <anyhttp/runtime.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail
{

//
// The application error codes of DoQ, RFC 9250, section 4.3.
//
enum class doq_error : std::uint64_t
{
   no_error = 0x0,
   internal_error = 0x1,
   protocol_error = 0x2,
   request_cancelled = 0x3,
   excessive_load = 0x4,
   unspecified_error = 0x5,
};

class doq_bridge final : public anyhttp_bridge
{
public:
   doq_bridge(const Executor& executor, ares_socket_t fd, int domain, const tls_options& options,
              const log_function& logger)
      : anyhttp_bridge(executor, fd, domain, options, logger, "DoQ")
   {
   }

private:
   void connect() override { anyhttp::launch(executor(), run(self<doq_bridge>())); }

   void on_query(std::string query) override
   {
      if (m_connection)
         start_exchange(std::move(query));
      else
         m_pending.push_back(std::move(query));
   }

   void on_stop() override
   {
      m_pending.clear();

      //
      // Let go of the connection, rather than just close it. anyhttp keeps the handler of the
      // accept_stream() that watch() waits in, and with that the bridge, until the connection has
      // gone through its closing period -- and the bridge is not to keep the connection in turn,
      // or the two keep each other where the context does not run that long. The opens that wait
      // for a stream fail with it.
      //
      auto connection = std::move(m_connection);
      connection.close(static_cast<std::uint64_t>(doq_error::no_error));
   }

   //
   // The connection, and then the queries that have waited for it.
   //
   static Task<void> run(std::shared_ptr<doq_bridge> self)
   {
      co_await anyhttp::yield_now(); // see anyhttp_bridge
      if (self->is_stopped())
         co_return;

      anyhttp::quic::Config config{
         .alpn = "doq",
         .tls_verify = self->options().verify,
         .reset_error_code = static_cast<std::uint64_t>(doq_error::request_cancelled),
         .stop_sending_error_code = static_cast<std::uint64_t>(doq_error::request_cancelled),
      };

      //
      // The endpoint of the local address is made the way the event loop makes every other one
      // from c-ares, and moved over into a UDP one the way anyhttp does that -- on Corosio, the
      // two are the same type. anyhttp's io::from_sockaddr() would do it in one go, but it is in
      // anyhttp/net.hpp, which is none of anyhttp's public headers and brings Asio's buffers along
      // on Corosio, into every translation unit that includes the resolver.
      //
      if (const auto& local = self->local_endpoint())
         config.local_endpoint = anyhttp::UdpEndpoint{local->address(), local->port()};

      //
      // To the address c-ares has for the server, which is all it has, with the certificate
      // checked against the name that came with the options -- or the chain only, without one.
      //
      // Unless the bridge is stopped first: when_either() cancels the connection that is being
      // made then, and waits for it to be through.
      //
      auto connected = co_await anyhttp::when_either(
         anyhttp::quic::connect(self->executor(), {self->server_endpoint()},
                                self->options().hostname, std::move(config)),
         self->stopped());
      if (connected.index() != 0)
         co_return;

      auto [ec, connection] = std::get<0>(std::move(connected));
      if (self->is_stopped())
      {
         connection.close(static_cast<std::uint64_t>(doq_error::no_error));
         co_return;
      }

      if (ec)
      {
         //
         // A certificate that is not taken shows up here as a refused connection, and why it was
         // not is in what anyhttp logs. c-ares only gets to see the end of its stream.
         //
         self->log(log_level::error, "connection to {} failed: {}", self->server(), ec.message());
         self->stop();
         co_return;
      }

      self->m_connection = std::move(connection);
      self->log(log_level::info, "established with {}, verified for '{}'{}", self->server(),
                self->options().hostname, self->options().verify ? "" : " NOT (verify is off)");

      anyhttp::launch(self->executor(), watch(self));

      for (auto& query : std::exchange(self->m_pending, {}))
         self->start_exchange(std::move(query));
   }

   //
   // A DoQ server opens no streams of its own (RFC 9250, section 4.2), so waiting for one is
   // waiting for the connection to end -- an idle timeout, the server closing it -- which has to
   // be the end of the stream for c-ares too, or it would go on sending into a connection that is
   // not there any more.
   //
   static Task<void> watch(std::shared_ptr<doq_bridge> self)
   {
      co_await anyhttp::yield_now(); // see anyhttp_bridge
      if (self->is_stopped())
         co_return;

      auto [ec, stream] = co_await self->m_connection.accept_stream();
      if (!ec)
      {
         self->log(log_level::error, "the server opened stream {}, which is a protocol error",
                   stream.id());
         self->m_connection.close(static_cast<std::uint64_t>(doq_error::protocol_error));
      }
      else
         self->log(log_level::debug, "connection over: {}", ec.message());

      self->stop();
   }

   void start_exchange(std::string query)
   {
      anyhttp::launch(executor(), exchange(self<doq_bridge>(), std::move(query)));
   }

   //
   // One query on a stream of its own. At the limit of streams the server allows, open_stream()
   // waits for it to allow another, and serves those that wait in the order they started -- which
   // is the order of the queries, as each exchange starts with the one yield.
   //
   static Task<void> exchange(std::shared_ptr<doq_bridge> self, std::string query)
   {
      co_await anyhttp::yield_now(); // see anyhttp_bridge
      if (self->is_stopped())
         co_return;

      auto [ec, stream] = co_await self->m_connection.open_stream();
      if (ec)
      {
         if (!self->is_stopped())
         {
            self->log(log_level::error, "cannot open a stream: {}", ec.message());
            self->stop();
         }
         co_return;
      }

      co_await self->ask(stream, std::move(query));
   }

   //
   // RFC 9250, section 4.2: the query with an ID of 0, which the stream makes unnecessary, and the
   // end of the stream after it. The answer is the one message on the stream the server sends
   // back, which gets the ID that c-ares chose -- that is how it tells its answers apart.
   //
   Task<void> ask(anyhttp::quic::Stream& stream, std::string query)
   {
      const std::array<char, 2> id{query[dns_prefix_size], query[dns_prefix_size + 1]};
      query[dns_prefix_size] = query[dns_prefix_size + 1] = 0;

      auto [write_ec, written] = co_await stream.write_eof(anyhttp::make_buffer(query));
      if (write_ec)
      {
         log(log_level::debug, "stream {}: sending the query failed: {}", stream.id(),
             write_ec.message());
         co_return; // c-ares asks again, or elsewhere, when its time is up
      }

      std::string answer;
      std::array<char, 4096> buffer{};
      for (;;)
      {
         auto [read_ec, n] = co_await stream.read_some(anyhttp::make_buffer(buffer));
         answer.append(buffer.data(), n);

         if (answer.size() > dns_prefix_size + dns_max_message_size)
         {
            protocol_error(stream, "more than one message");
            co_return;
         }

         if (read_ec == anyhttp::errors::eof)
            break;

         if (read_ec)
         {
            log(log_level::debug, "stream {}: receiving the answer failed: {}", stream.id(),
                read_ec.message());
            co_return;
         }
      }

      if (answer.size() < dns_prefix_size + dns_header_size)
      {
         protocol_error(stream, std::format("an answer of {} bytes", answer.size()));
         co_return;
      }

      const std::size_t size = (std::size_t{static_cast<unsigned char>(answer[0])} << 8) |
                               static_cast<unsigned char>(answer[1]);
      if (size != answer.size() - dns_prefix_size)
      {
         protocol_error(stream, std::format("{} bytes with a length of {}",
                                            answer.size() - dns_prefix_size, size));
         co_return;
      }

      answer[dns_prefix_size] = id[0];
      answer[dns_prefix_size + 1] = id[1];
      anyhttp_bridge::answer(answer);
   }

   //
   // RFC 9250, section 4.3.3: a stream that does not carry what it has to is an error of the
   // connection, not just of the stream.
   //
   void protocol_error(const anyhttp::quic::Stream& stream, const std::string& what)
   {
      log(log_level::error, "stream {}: {}, which is a protocol error", stream.id(), what);
      m_connection.close(static_cast<std::uint64_t>(doq_error::protocol_error));
      stop();
   }

   anyhttp::quic::Connection m_connection;
   std::vector<std::string> m_pending; // queries waiting for the connection
};

} // namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail

#endif // C_ARES_ASYNC_ANYHTTP
