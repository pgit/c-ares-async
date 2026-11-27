// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// A DNS server on the loopback interface that answers from a table, for a resolver to be pointed
// at. It speaks UDP, TCP and, once it has a certificate, DNS over TLS -- all on one port, as
// c-ares has a single port for a server whichever way it asks. Where the tests are built with
// anyhttp, it speaks DNS over QUIC and DNS over HTTPS as well, with anyhttp's server, each on a
// port of its own -- and on the runtime anyhttp is built for, which is the one of the tests, where
// the rest is on Asio.
//
// The messages are taken apart and put together by c-ares' own record functions, so the tests do
// not come with a second implementation of the wire format to keep right.
//
#pragma once

#include "test_certificate.hpp"

#include <boost/asio/any_io_executor.hpp>

#if C_ARES_ASYNC_ANYHTTP
#include <anyhttp/common.hpp>
#include <anyhttp/runtime.hpp>
#endif

#include <chrono>

#include <ares.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class fake_dns_server
{
public:
   enum class transport
   {
      udp,
      tcp,
      tls,
      quic,
      https
   };

   //
   // One question the server was asked.
   //
   struct query
   {
      std::string name;
      ares_dns_rec_type_t type;
      transport via;
      std::string from; // the client's address, without the port -- unknown over HTTPS
   };

   //
   // One request to the server for DNS over HTTPS, and the ID of the message in its body.
   //
   struct https_request
   {
      std::string method;
      std::string path;
      std::string content_type;
      std::string accept;
      unsigned id;
   };

   //
   // One SvcParam of an HTTPS record, with its value in wire format.
   //
   struct svc_param
   {
      unsigned short key;
      std::vector<unsigned char> value;
   };

   explicit fake_dns_server(const boost::asio::any_io_executor& executor);
   ~fake_dns_server();

   fake_dns_server(const fake_dns_server&) = delete;
   fake_dns_server& operator=(const fake_dns_server&) = delete;

   //
   // Where the server listens, the way resolver::set_servers() takes it: "127.0.0.1:<port>".
   //
   std::string address() const;

   //
   // The zone. A name that has any record at all exists, which makes the difference between an
   // empty answer and NXDOMAIN for the record types it does not have.
   //
   void add_address(std::string_view name, std::string_view address); // A or AAAA
   void add_cname(std::string_view name, std::string_view target);
   void add_https(std::string_view name, unsigned short priority, std::string_view target,
                  std::vector<svc_param> params = {});

   //
   // Ways of not being helpful: swallowing every question, answering all of them with an error,
   // or claiming over UDP that the answer did not fit, which sends the resolver to TCP.
   //
   void set_silent(bool silent);
   void set_rcode(ares_dns_rcode_t rcode);
   void set_truncate_udp(bool truncate);

   //
   // Makes every connection from here on start with a TLS handshake, the server presenting
   // 'certificate', and picking 'alpn' where the client offers it -- and breaking off the
   // handshake where the client offers others only. Without 'alpn' it does no ALPN, and a client
   // offering some gets none, as from most servers.
   //
   void use_tls(const test_certificate& certificate, std::string alpn = {});

#if C_ARES_ASYNC_ANYHTTP
   //
   // Starts serving DNS over QUIC on a port of its own, the server presenting 'certificate',
   // dropping a connection that has been quiet for 'idle_timeout', and allowing a client
   // 'max_streams' streams at once. It runs on 'executor', which is anyhttp's runtime's: the one
   // the server was made with on Asio, and the resolver's loop on Corosio, which the tests step
   // from the same thread as the server's.
   //
   void use_quic(const anyhttp::Executor& executor, const test_certificate& certificate,
                 std::chrono::nanoseconds idle_timeout = std::chrono::seconds(30),
                 std::uint64_t max_streams = 100);

   //
   // Where it serves DNS over QUIC, the way resolver::set_servers() takes it.
   //
   std::string quic_address() const;

   //
   // Stops serving, all of it, as the destructor does -- for a context that is to run out of work
   // before it goes. A QUIC connection takes its closing period to end, and anyhttp keeps what it
   // waits for until then, which is not freed where the context goes away before that.
   //
   void shutdown();

   //
   // A way of breaking the protocol: a second message on the stream, after the answer.
   //
   void set_quic_extra_message(bool extra);

   //
   // A way of keeping a client waiting: the query is read, and its stream kept open with no answer
   // on it, until the server shuts down -- where set_silent() resets the stream, which lets the
   // client open another in its place.
   //
   void set_quic_hold(bool hold);

   std::size_t quic_connections() const; // that negotiated "doq"
   const std::vector<unsigned>& quic_ids() const; // the message ID of every query, in order

   //
   // Starts serving DNS over HTTPS on a port of its own, the server presenting 'certificate',
   // answering POSTs to 'path', and allowing a client 'max_streams' requests at once over HTTP/2.
   // 'protocol' is the most it speaks: HTTP/2 and HTTP/1.1, preferring HTTP/2, or HTTP/1.1 only,
   // as a server without HTTP/2 does. On 'executor', as for use_quic().
   //
   void use_https(const anyhttp::Executor& executor, const test_certificate& certificate,
                  std::string path = "/dns-query", std::uint64_t max_streams = 100,
                  anyhttp::Protocol protocol = anyhttp::Protocol::h2);

   //
   // Where it serves DNS over HTTPS, the way resolver::set_servers() takes it.
   //
   std::string https_address() const;

   //
   // Ends every connection, and listens again on the same port, as a server that has gone away
   // and come back.
   //
   void restart_https();

   //
   // Ways of not being a DoH server: answering every request with 'status' and no body, and
   // answering the queries with a body of another 'type'.
   //
   void set_https_status(unsigned status);
   void set_https_content_type(std::string type);

   //
   // As set_quic_hold(): the query is read, and its request kept with no response, until the
   // server shuts down.
   //
   void set_https_hold(bool hold);

   const std::vector<https_request>& https_requests() const;
#endif

   //
   // What happened so far.
   //
   const std::vector<query>& queries() const;
   std::size_t count(std::string_view name, ares_dns_rec_type_t type) const;
   std::size_t count(transport via) const;
   std::size_t connections() const; // accepted, whatever became of them
   std::size_t handshakes() const; // TLS handshakes that went through
   const std::vector<std::string>& alpn_offered() const; // in ALPN, by the last TLS client

private:
   struct state;
   std::shared_ptr<state> m_state; // shared with the coroutines that do the serving
};
