// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The c-ares side of a socket of the anyhttp transport, see detail/anyhttp_transport.hpp: one TCP
// connection that c-ares believes it has, and what the protocol makes of it. Only where
// C_ARES_ASYNC_ANYHTTP is 1, as the transport.
//
#pragma once

#include <c_ares_async/runtime.hpp>

#if C_ARES_ASYNC_ANYHTTP

#include <c_ares_async/detail/event_loop.hpp>
#include <c_ares_async/logging.hpp>
#include <c_ares_async/tls_options.hpp>

#include <anyhttp/runtime.hpp>

#include <ares.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail
{

//
// A DNS message on a stream, as c-ares sends it over TCP and DoQ sends it on a QUIC stream: two
// bytes of length, then the message, of which the first two bytes are the ID. Nothing shorter
// than the twelve bytes of a header is a message, and the length allows 65535 at most.
//
inline constexpr std::size_t dns_prefix_size = 2;
inline constexpr std::size_t dns_header_size = 12;
inline constexpr std::size_t dns_max_message_size = 65535;

//
// One TCP connection c-ares believes it has, and the connection of anyhttp's that answers for it.
//
// c-ares' side of it is the socket functions, which come here from inside c-ares, and the
// readiness the resolver waits on, see anyhttp_transport::descriptor. That is all here. The
// protocol's side is coroutines, which take a shared_ptr to the bridge: that is what keeps it
// around, as c-ares closing its socket only stops it. They are in the classes that derive from
// this one, which say how to connect, what to do with a query and how to end the connection. All
// of it runs on the resolver's executor, so it needs no lock.
//
// It is written once, for either runtime. What it does on the runtime is anyhttp's runtime layer
// -- starting coroutines, posting, waiting for one of two things -- as the bridge is only ever
// compiled along with anyhttp, which has that layer for both.
//
// Every coroutine of a bridge yields before it does anything else. anyhttp::launch() starts one
// from the executor's queue on Asio, as co_spawn() does, but right away on Corosio, where the
// caller is on the executor already -- and what launches them is c-ares, in the middle of
// aconnect(), asendto() or aclose(). This way, they start the same way on both runtimes, after
// c-ares is through.
//
class anyhttp_bridge : public std::enable_shared_from_this<anyhttp_bridge>
{
public:
   //
   // 'protocol' is what the log messages call it: "DoQ" or "DoH".
   //
   anyhttp_bridge(const Executor& executor, ares_socket_t fd, int domain,
                  const tls_options& options, const log_function& logger, std::string_view protocol)
      : m_executor(executor), m_fd(fd), m_domain(domain), m_options(options), m_logger(&logger),
        m_protocol(protocol)
   {
   }

   virtual ~anyhttp_bridge() = default;

   anyhttp_bridge(const anyhttp_bridge&) = delete;
   anyhttp_bridge& operator=(const anyhttp_bridge&) = delete;

   int domain() const noexcept { return m_domain; }

   //
   // Where the connection is to come from, which is what c-ares binds its socket to before it
   // connects, where the channel has a local address of the socket's family. It is anyhttp's
   // socket that is bound, once there is one: an address that is not this host's fails the
   // connection, not this.
   //
   int bind(const struct sockaddr* address, ares_socklen_t length) noexcept
   {
      m_local = make_endpoint(address, length);
      if (!m_local)
      {
         errno = EAFNOSUPPORT;
         return -1;
      }

      return 0;
   }

   //
   // Connects to 'server', and passes the queries on once there is a connection.
   //
   void start(const TcpEndpoint& server)
   {
      m_server = server;
      connect();
   }

   //
   // What c-ares sends: all of it, every time. The messages in it go to the protocol as soon as
   // they are complete, which keeps them until there is a connection -- which is where a TCP
   // connection would have them wait, too, in its send buffer.
   //
   ares_ssize_t send(const void* data, std::size_t size) noexcept
   {
      if (m_stopped)
      {
         errno = ECONNRESET;
         return -1;
      }

      //
      // Nothing is to be thrown back into c-ares, which is C. What runs out of memory here is in
      // the middle of a message, and the stream c-ares has written it to is no good after that.
      //
      try
      {
         m_sent.append(static_cast<const char*>(data), size);
         while (m_sent.size() >= dns_prefix_size)
         {
            const std::size_t length = (std::size_t{static_cast<unsigned char>(m_sent[0])} << 8) |
                                       static_cast<unsigned char>(m_sent[1]);
            if (length < dns_header_size)
            {
               log(log_level::error, "c-ares sent a message of {} bytes", length);
               stop();
               errno = ECONNRESET;
               return -1;
            }

            if (m_sent.size() < dns_prefix_size + length)
               break;

            auto query = m_sent.substr(0, dns_prefix_size + length);
            m_sent.erase(0, dns_prefix_size + length);
            on_query(std::move(query));
         }
      }
      catch (...)
      {
         stop();
         errno = ENOMEM;
         return -1;
      }

      return static_cast<ares_ssize_t>(size);
   }

   //
   // The answers, in the order they came in, for as much of them as c-ares has room for -- and
   // once they are all read and the connection is over, the end of the stream.
   //
   ares_ssize_t receive(void* data, std::size_t size) noexcept
   {
      if (!m_received.empty())
      {
         const auto n = std::min(size, m_received.size());
         std::memcpy(data, m_received.data(), n);
         m_received.erase(0, n);
         return static_cast<ares_ssize_t>(n);
      }

      if (m_stopped)
         return 0;

      errno = EAGAIN;
      return -1;
   }

   //
   // The waits of anyhttp_transport::descriptor. There is one for reading at a time, as the
   // resolver has one at a time in each direction; one for writing is ready right away, as send()
   // takes everything.
   //
   void wait(wait_type type, std::function<void(const error_code&)> on_ready)
   {
      if (type == wait_type::write || readable())
         complete(std::move(on_ready), {});
      else
         m_on_readable = std::move(on_ready);
   }

   void cancel_wait()
   {
      if (m_on_readable)
         complete(std::exchange(m_on_readable, nullptr), errors::canceled);
   }

   //
   // Ends it all: the connection that is being made, the connection with what is open on it, and
   // the queries that have not gone out. c-ares reads the end of its stream once it has read what
   // is there. Called by c-ares closing its socket as well as by whatever went wrong here;
   // whichever is first does it.
   //
   // From here on, nothing looks at the resolver's logger any more, which may be gone.
   //
   void stop()
   {
      if (m_stopped)
         return;

      m_stopped = true;
      m_logger = nullptr;
      m_sent.clear();
      m_stop.set(); // which ends the connection that is being made, if it still is
      on_stop();
      notify();
   }

protected:
   //
   // What the protocol does: start connecting to server(), take a query, which is a message with
   // its two bytes of length as c-ares has sent it, and end the connection once stop() is called.
   // None of them may throw, as they are called from inside c-ares, and the first two do not run
   // once the bridge is stopped.
   //
   virtual void connect() = 0;
   virtual void on_query(std::string query) = 0;
   virtual void on_stop() = 0;

   //
   // Hands c-ares an answer: two bytes of length, then the message, with the ID it gave the query.
   //
   void answer(std::string_view answer)
   {
      if (m_stopped)
         return;

      m_received += answer;
      notify();
   }

   template <typename... Args>
   void log(log_level level, std::format_string<Args...> format, Args&&... args)
   {
      if (m_logger && *m_logger)
         (*m_logger)(level, std::format("fd={} {} ", m_fd, m_protocol) +
                               std::format(format, std::forward<Args>(args)...));
   }

   //
   // The shared_ptr to the derived class that a coroutine of it keeps the bridge alive with.
   //
   template <typename Derived>
   std::shared_ptr<Derived> self()
   {
      return std::static_pointer_cast<Derived>(shared_from_this());
   }

   const Executor& executor() const noexcept { return m_executor; }
   const tls_options& options() const noexcept { return m_options; }
   const TcpEndpoint& server_endpoint() const noexcept { return m_server; }
   const std::optional<TcpEndpoint>& local_endpoint() const noexcept { return m_local; }
   bool is_stopped() const noexcept { return m_stopped; }

   std::string server() const
   {
      return m_server.address().is_v6()
                ? std::format("[{}]:{}", m_server.address().to_string(), m_server.port())
                : std::format("{}:{}", m_server.address().to_string(), m_server.port());
   }

   //
   // Comes back once stop() has been called, or when it is cancelled. A task, for when_either()
   // to take: what Event::wait() returns is one on Asio, and an awaitable of Capy's on Corosio.
   //
   Task<void> stopped() { std::ignore = co_await m_stop.wait(); }

private:
   bool readable() const noexcept { return !m_received.empty() || m_stopped; }

   //
   // From the executor's queue, see anyhttp_transport::descriptor.
   //
   void complete(std::function<void(const error_code&)> on_ready, const error_code& ec)
   {
      anyhttp::run_later(m_executor, [on_ready = std::move(on_ready), ec] { on_ready(ec); });
   }

   //
   // Completes the wait for reading, where there is one, if there is something to read now.
   //
   void notify()
   {
      if (m_on_readable && readable())
         complete(std::exchange(m_on_readable, nullptr), {});
   }

   Executor m_executor;
   ares_socket_t m_fd; // c-ares' descriptor, which is what the log messages go by
   int m_domain;
   tls_options m_options;
   const log_function* m_logger; // null once stopped
   std::string_view m_protocol;
   TcpEndpoint m_server;
   std::optional<TcpEndpoint> m_local; // what c-ares bound its socket to, if anything
   anyhttp::Event m_stop; // set by stop(), for the connection that is being made
   std::string m_sent; // what c-ares has sent of a message that is not complete yet
   std::string m_received; // answers that c-ares has not read yet
   std::function<void(const error_code&)> m_on_readable; // the resolver's wait for reading
   bool m_stopped = false;
};

} // namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail

#endif // C_ARES_ASYNC_ANYHTTP
