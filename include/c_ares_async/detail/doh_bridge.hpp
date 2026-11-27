// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// DNS over HTTPS (RFC 8484) on a socket of the anyhttp transport, with anyhttp's HTTP client: one
// HTTP/2 session for the TCP connection c-ares believes it has, and a POST of its own for every
// query. Only where C_ARES_ASYNC_ANYHTTP is 1, as the transport.
//
// https://www.rfc-editor.org/rfc/rfc8484
//
#pragma once

#include <c_ares_async/runtime.hpp>

#if C_ARES_ASYNC_ANYHTTP

#include <c_ares_async/detail/anyhttp_bridge.hpp>

#include <anyhttp/client.hpp>
#include <anyhttp/runtime.hpp>
#include <anyhttp/session.hpp>

#include <boost/http/field.hpp>
#include <boost/url/url.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail
{

//
// What a DNS message is as the body of a request or a response, RFC 8484, section 6.
//
inline constexpr std::string_view dns_message_type = "application/dns-message";

//
// Whether a Content-Type is that, whatever the case, and with whatever parameters after it.
//
inline bool is_dns_message(std::string_view content_type) noexcept
{
   content_type = content_type.substr(0, content_type.find(';'));
   while (!content_type.empty() && (content_type.back() == ' ' || content_type.back() == '\t'))
      content_type.remove_suffix(1);

   return content_type.size() == dns_message_type.size() &&
          std::equal(content_type.begin(), content_type.end(), dns_message_type.begin(),
                     [](char a, char b) {
                        return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
                     });
}

class doh_bridge final : public anyhttp_bridge
{
public:
   doh_bridge(const Executor& executor, ares_socket_t fd, int domain, const tls_options& options,
              const log_function& logger)
      : anyhttp_bridge(executor, fd, domain, options, logger, "DoH")
   {
   }

private:
   //
   // A query waiting to be sent, with its two bytes of length, and whether it has been before.
   //
   struct pending
   {
      std::string query;
      bool again = false;
   };

   void connect() override { anyhttp::launch(executor(), run(self<doh_bridge>())); }

   //
   // The queries wait here, for the session and for the requests the server allows at a time. The
   // one coroutine that sends them, send(), takes them in the order they came.
   //
   void on_query(std::string query) override
   {
      m_queue.push_back({.query = std::move(query)});
      m_wake.set();
   }

   //
   // The queries that have not gone out are dropped, and send() comes back. The session ends the
   // orderly way, with a GOAWAY and a "close_notify", which fails the requests that are still out:
   // that takes a coroutine, which keeps the bridge until it is through, as long as that takes --
   // two seconds at most for each of the waits it has, see anyhttp's Session::shutdown().
   //
   void on_stop() override
   {
      m_queue.clear();
      m_wake.set();
      if (m_session)
         anyhttp::launch(executor(), shutdown(self<doh_bridge>()));
   }

   //
   // The URL the queries go to: the path of the options, on the server's name or, without one,
   // its address, which is then what the certificate has to be valid for. The address is the one
   // c-ares has, and the connection goes there whatever the name -- anyhttp does not resolve it.
   //
   static boost::urls::url make_url(const tls_options& options, const TcpEndpoint& server)
   {
      boost::urls::url url;
      url.set_scheme_id(boost::urls::scheme::https);
      url.set_host_address(options.hostname.empty() ? server.address().to_string()
                                                    : options.hostname);
      if (server.port() != 443)
         url.set_port_number(server.port());
      url.set_path(options.path);
      return url;
   }

   //
   // RFC 8484, section 4.1: a POST with the message as its body, and the type of that said.
   //
   static anyhttp::Fields fields(std::size_t length)
   {
      anyhttp::Fields fields;
      fields.set(boost::http::field::content_type, dns_message_type);
      fields.set(boost::http::field::accept, dns_message_type);
      fields.set(boost::http::field::content_length, std::to_string(length));
      return fields;
   }

   //
   // The session, and then the queries, those that have waited for it first.
   //
   static Task<void> run(std::shared_ptr<doh_bridge> self)
   {
      co_await anyhttp::yield_now(); // see anyhttp_bridge
      if (self->is_stopped())
         co_return;

      //
      // To the address c-ares has for the server, as HTTP/2 -- or HTTP/1.1, where that is all the
      // server says it speaks (RFC 8484, section 5.2, recommends HTTP/2 at least).
      //
      self->m_url = make_url(self->options(), self->server_endpoint());
      self->m_client.emplace(self->executor(),
                             anyhttp::client::Config{.url = self->m_url,
                                                     .endpoints = {self->server_endpoint()},
                                                     .local_endpoint = self->local_endpoint(),
                                                     .protocol = anyhttp::Protocol::h2,
                                                     .tls_verify = self->options().verify});

      //
      // Unless the bridge is stopped first: when_either() cancels the connection that is being
      // made then, and waits for it to be through.
      //
      auto connected = co_await anyhttp::when_either(self->m_client->connect(), self->stopped());
      if (connected.index() != 0)
         co_return;

      auto [ec, session] = std::get<0>(std::move(connected));
      if (self->is_stopped())
         co_return; // which cuts the session, if there is one

      if (ec)
      {
         //
         // A certificate that is not taken shows up here as a failed handshake, and why it was
         // not is in what anyhttp logs. c-ares only gets to see the end of its stream.
         //
         self->log(log_level::error, "connection to {} failed: {}", self->server(), ec.message());
         self->stop();
         co_return;
      }

      self->m_session = std::move(session);
      self->m_one_at_a_time = self->m_session.protocol() == anyhttp::Protocol::h1;
      self->log(log_level::info, "established with {} over {}, verified for '{}'{}", self->server(),
                self->m_one_at_a_time ? "HTTP/1.1" : "HTTP/2", self->m_url.host_address(),
                self->options().verify ? "" : " NOT (verify is off)");

      co_await send(self);
   }

   //
   // Submits the queries, one after the other, as long as the server takes them: a session limits
   // the requests that are out at a time -- to one over HTTP/1.1, and to what the server says over
   // HTTP/2 -- and says so with would_block rather than waiting. Then the next one waits for an
   // answer to come in, see exchange(). Over HTTP/1.1, the next one waits for the answer to the
   // last as well: it would come in after that one anyway, and the session takes the request but
   // has no response for it until then.
   //
   // A session that cannot take a request at all is over, which is what c-ares learns from it as
   // the end of its stream. It reconnects then, and sends again what it has not had answers for.
   // That is how it learns as well about a session that has ended while there was nothing to send
   // -- the server closing it after a while, say: there is nothing of anyhttp's to wait for that.
   //
   static Task<void> send(std::shared_ptr<doh_bridge> self)
   {
      for (;;)
      {
         self->m_wake.clear(); // before looking for work, see anyhttp's Event
         if (self->is_stopped())
            co_return;

         if (self->m_queue.empty() || self->m_blocked ||
             (self->m_one_at_a_time && self->m_in_flight > 0))
         {
            std::ignore = co_await self->m_wake.wait();
            continue;
         }

         const auto length = self->m_queue.front().query.size() - dns_prefix_size;
         auto [ec, request] = co_await self->m_session.submit("POST", self->m_url, fields(length));
         if (self->is_stopped())
            co_return;

         if (ec == anyhttp::errors::would_block && self->m_in_flight > 0)
         {
            self->log(log_level::debug, "{} queries out, as many as the server takes",
                      self->m_in_flight);
            self->m_blocked = true;
            continue;
         }

         if (ec)
         {
            self->log(log_level::debug, "connection over: {}", ec.message());
            self->stop();
            co_return;
         }

         auto query = std::move(self->m_queue.front());
         self->m_queue.pop_front();
         ++self->m_in_flight;
         anyhttp::launch(self->executor(), exchange(self, std::move(request), std::move(query)));
      }
   }

   //
   // One query, on the request that send() has submitted for it -- and then room for the next.
   //
   // A stream that ends before there is a response, on a session that goes on, is one that the
   // server has reset, and the query goes out again, first in line -- once, so that a server that
   // resets every one of them is not asked forever. The server has every right to: until its
   // SETTINGS are in, the session does not know how many requests it allows at a time, and sends
   // what it has, and the server refuses those it does not allow (RFC 9113, sections 5.1.2 and
   // 8.7). anyhttp does not say why a stream was reset, so it is the same for every reason --
   // which is no harm, as a query can be asked again.
   //
   static Task<void> exchange(std::shared_ptr<doh_bridge> self, anyhttp::client::Request request,
                              pending query)
   {
      co_await anyhttp::yield_now(); // see anyhttp_bridge
      bool answered = true;
      if (!self->is_stopped())
         answered = co_await self->ask(request, query.query);

      --self->m_in_flight;
      self->m_blocked = false;
      if (!answered && !self->is_stopped() && !query.again)
      {
         self->log(log_level::debug, "sending the query again");
         self->m_queue.push_front({.query = std::move(query.query), .again = true});
      }
      self->m_wake.set();
   }

   //
   // RFC 8484, section 4.1: the query without its two bytes of length, and with an ID of 0, which
   // is what a cache can take for the same question each time. The answer is the body of a
   // response with a status of 2xx (section 4.2.1), and gets the ID that c-ares chose -- that is
   // how it tells its answers apart -- and the two bytes of length back.
   //
   // A status that is not 2xx says that the server does not answer queries at that URL, or not
   // now, or not this one; and an answer that is not a DNS message, that it is no DoH server at
   // all. Either one is the end of the connection for c-ares, which fails the server the way it
   // does for TCP, rather than waiting for its time to be up -- for every query, as it would be.
   //
   // Comes back with whether the stream has come as far as a response -- false for one that has
   // not, and the query may go out again.
   //
   Task<bool> ask(anyhttp::client::Request& request, const std::string& query)
   {
      const std::array<char, 2> id{query[dns_prefix_size], query[dns_prefix_size + 1]};
      std::string body = query.substr(dns_prefix_size);
      body[0] = body[1] = 0;

      auto [write_ec, written] = co_await request.write_eof(anyhttp::make_buffer(body));
      if (write_ec)
      {
         log(log_level::debug, "sending the query failed: {}", write_ec.message());
         co_return false;
      }

      auto [response_ec, response] = co_await request.get_response();
      if (response_ec)
      {
         log(log_level::debug, "receiving the answer failed: {}", response_ec.message());
         co_return false;
      }

      if (const auto status = response.status_code(); status < 200 || status > 299)
      {
         give_up(std::format("the server answers a query with status {}", status));
         co_return true;
      }

      if (const auto type = response.fields().value_or(boost::http::field::content_type, "");
          !is_dns_message(type))
      {
         give_up(std::format("the server answers a query with a body of type '{}'", type));
         co_return true;
      }

      std::string answer(dns_prefix_size, '\0');
      std::array<char, 4096> buffer{};
      for (;;)
      {
         auto [read_ec, n] = co_await response.read_some(anyhttp::make_buffer(buffer));
         answer.append(buffer.data(), n);

         if (answer.size() > dns_prefix_size + dns_max_message_size)
         {
            give_up(std::format("the server answers a query with more than {} bytes",
                                dns_max_message_size));
            co_return true;
         }

         if (read_ec == anyhttp::errors::eof)
            break;

         if (read_ec)
         {
            log(log_level::debug, "receiving the answer failed: {}", read_ec.message());
            co_return true;
         }
      }

      const std::size_t size = answer.size() - dns_prefix_size;
      if (size < dns_header_size)
      {
         give_up(std::format("the server answers a query with {} bytes", size));
         co_return true;
      }

      answer[0] = static_cast<char>(size >> 8);
      answer[1] = static_cast<char>(size & 0xff);
      answer[dns_prefix_size] = id[0];
      answer[dns_prefix_size + 1] = id[1];
      anyhttp_bridge::answer(answer);
      co_return true;
   }

   void give_up(const std::string& why)
   {
      log(log_level::error, "{}, giving up on {}", why, server());
      stop();
   }

   static Task<void> shutdown(std::shared_ptr<doh_bridge> self)
   {
      co_await anyhttp::yield_now(); // see anyhttp_bridge
      std::ignore = co_await self->m_session.shutdown();
   }

   boost::urls::url m_url; // where the queries go
   std::optional<anyhttp::client::Client> m_client; // what makes the session
   anyhttp::Session m_session;
   bool m_one_at_a_time = false; // HTTP/1.1: one request at a time, answer included
   std::deque<pending> m_queue;
   std::size_t m_in_flight = 0; // queries sent and not answered yet
   bool m_blocked = false; // the session has taken what it can, until an answer comes in
   anyhttp::Event m_wake; // for send(): a query, an answer or the end
};

} // namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail

#endif // C_ARES_ASYNC_ANYHTTP
