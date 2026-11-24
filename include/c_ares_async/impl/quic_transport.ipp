// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <c_ares_async/runtime.hpp>

#if C_ARES_ASYNC_DOQ // as detail/quic_transport.hpp: empty without

#include <c_ares_async/detail/event_loop.hpp>
#include <c_ares_async/detail/log.hpp>
#include <c_ares_async/detail/quic_transport.hpp> // NOLINT(misc-header-include-cycle)
#include <c_ares_async/detail/socket_functions.hpp>

#include <anyhttp/quic.hpp>
#include <anyhttp/runtime.hpp>

#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail
{

//
// A DNS message on a stream, as c-ares sends it over TCP and DoQ sends it on a QUIC stream: two
// bytes of length, then the message, of which the first two bytes are the ID. Nothing shorter
// than the twelve bytes of a header is a message, and the length allows 65535 at most.
//
inline constexpr std::size_t doq_prefix_size = 2;
inline constexpr std::size_t dns_header_size = 12;
inline constexpr std::size_t dns_max_message_size = 65535;

// --------------------------------------------------------------------------------------------------

//
// One TCP connection c-ares believes it has, and the QUIC connection that answers for it.
//
// c-ares' side of it is the socket functions, which come here from inside c-ares, and the
// readiness the resolver waits on, see quic_transport::descriptor. The QUIC side is coroutines,
// which take a shared_ptr to the bridge: that is what keeps it around, as c-ares closing its
// socket only stops it. All of it runs on the resolver's executor, so it needs no lock.
//
// It is written once, for either runtime. What it does on the runtime is anyhttp's runtime layer
// -- starting coroutines, posting, waiting for one of two things -- as the bridge is only ever
// compiled along with anyhttp, which has that layer for both.
//
// Every coroutine here yields before it does anything else. anyhttp::launch() starts one from the
// executor's queue on Asio, as co_spawn() does, but right away on Corosio, where the caller is on
// the executor already -- and what launches them here is c-ares, in the middle of aconnect() or
// asendto(). This way, they start the same way on both runtimes, after c-ares is through.
//
class quic_transport::bridge : public std::enable_shared_from_this<bridge>
{
public:
   bridge(const Executor& executor, ares_socket_t fd, int domain, const tls_options& options,
          const log_function& logger)
      : m_executor(executor), m_fd(fd), m_domain(domain), m_options(options), m_logger(&logger)
   {
   }

   int domain() const noexcept { return m_domain; }

   //
   // Connects to 'server', and passes the queries on once there is a connection.
   //
   void start(const TcpEndpoint& server)
   {
      m_server = server;
      anyhttp::launch(m_executor, run(shared_from_this()));
   }

   //
   // What c-ares sends: all of it, every time. The messages in it go out as soon as they are
   // complete and there is a connection, and until then they wait here -- which is where a TCP
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
         while (m_sent.size() >= doq_prefix_size)
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

            if (m_sent.size() < doq_prefix_size + length)
               break;

            auto query = m_sent.substr(0, doq_prefix_size + length);
            m_sent.erase(0, doq_prefix_size + length);
            if (m_connection)
               start_exchange(std::move(query));
            else
               m_pending.push_back(std::move(query));
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
   // The waits of quic_transport::descriptor. There is one for reading at a time, as the resolver
   // has one at a time in each direction; one for writing is ready right away, as send() takes
   // everything.
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
   // Ends it all: the connection that is being made, the QUIC connection with what is open on it,
   // and the queries that have not gone out. c-ares reads the end of its stream once it has read
   // what is there. Called by c-ares closing its socket as well as by whatever went wrong here;
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
      m_pending.clear();
      m_stop.set(); // which ends the connection that is being made, if it still is

      //
      // Let go of the connection, rather than just close it. anyhttp keeps the handler of the
      // accept_stream() that watch() waits in, and with that the bridge, until the connection has
      // gone through its closing period -- and the bridge is not to keep the connection in turn,
      // or the two keep each other where the context does not run that long. The opens that wait
      // for a stream fail with it.
      //
      auto connection = std::move(m_connection);
      connection.close(static_cast<std::uint64_t>(doq_error::no_error));

      notify();
   }

private:
   template <typename... Args>
   void log(log_level level, std::format_string<Args...> format, Args&&... args)
   {
      if (m_logger && *m_logger)
         (*m_logger)(level, std::format("fd={} DoQ ", m_fd) +
                               std::format(format, std::forward<Args>(args)...));
   }

   std::string server() const
   {
      return std::format("{}:{}", m_server.address().to_string(), m_server.port());
   }

   bool readable() const noexcept { return !m_received.empty() || m_stopped; }

   //
   // From the executor's queue, see quic_transport::descriptor.
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

   //
   // Comes back once stop() has been called, or when it is cancelled. A task, for when_either()
   // to take: what Event::wait() returns is one on Asio, and an awaitable of Capy's on Corosio.
   //
   Task<void> stopped() { std::ignore = co_await m_stop.wait(); }

   //
   // The connection, and then the queries that have waited for it.
   //
   static Task<void> run(std::shared_ptr<bridge> self)
   {
      co_await anyhttp::yield_now(); // see the class
      if (self->m_stopped)
         co_return;

      anyhttp::quic::Config config{
         .alpn = "doq",
         .tls_verify = self->m_options.verify,
         .reset_error_code = static_cast<std::uint64_t>(doq_error::request_cancelled),
         .stop_sending_error_code = static_cast<std::uint64_t>(doq_error::request_cancelled),
      };

      //
      // To the address c-ares has for the server, which is all it has, with the certificate
      // checked against the name that came with the options -- or the chain only, without one.
      //
      // Unless the bridge is stopped first: when_either() cancels the connection that is being
      // made then, and waits for it to be through.
      //
      auto connected = co_await anyhttp::when_either(
         anyhttp::quic::connect(self->m_executor, {self->m_server}, self->m_options.hostname,
                                std::move(config)),
         self->stopped());
      if (connected.index() != 0)
         co_return;

      auto [ec, connection] = std::get<0>(std::move(connected));
      if (self->m_stopped)
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
                self->m_options.hostname, self->m_options.verify ? "" : " NOT (verify is off)");

      anyhttp::launch(self->m_executor, watch(self));

      for (auto& query : std::exchange(self->m_pending, {}))
         self->start_exchange(std::move(query));
   }

   //
   // A DoQ server opens no streams of its own (RFC 9250, section 4.2), so waiting for one is
   // waiting for the connection to end -- an idle timeout, the server closing it -- which has to
   // be the end of the stream for c-ares too, or it would go on sending into a connection that is
   // not there any more.
   //
   static Task<void> watch(std::shared_ptr<bridge> self)
   {
      co_await anyhttp::yield_now(); // see the class
      if (self->m_stopped)
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
      anyhttp::launch(m_executor, exchange(shared_from_this(), std::move(query)));
   }

   //
   // One query on a stream of its own. At the limit of streams the server allows, open_stream()
   // waits for it to allow another, and serves those that wait in the order they started -- which
   // is the order of the queries, as each exchange starts with the one yield.
   //
   static Task<void> exchange(std::shared_ptr<bridge> self, std::string query)
   {
      co_await anyhttp::yield_now(); // see the class
      if (self->m_stopped)
         co_return;

      auto [ec, stream] = co_await self->m_connection.open_stream();
      if (ec)
      {
         if (!self->m_stopped)
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
      const std::array<char, 2> id{query[doq_prefix_size], query[doq_prefix_size + 1]};
      query[doq_prefix_size] = query[doq_prefix_size + 1] = 0;

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

         if (answer.size() > doq_prefix_size + dns_max_message_size)
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

      if (answer.size() < doq_prefix_size + dns_header_size)
      {
         protocol_error(stream, std::format("an answer of {} bytes", answer.size()));
         co_return;
      }

      const std::size_t size = (std::size_t{static_cast<unsigned char>(answer[0])} << 8) |
                               static_cast<unsigned char>(answer[1]);
      if (size != answer.size() - doq_prefix_size)
      {
         protocol_error(stream, std::format("{} bytes with a length of {}",
                                            answer.size() - doq_prefix_size, size));
         co_return;
      }

      if (m_stopped)
         co_return;

      answer[doq_prefix_size] = id[0];
      answer[doq_prefix_size + 1] = id[1];
      m_received += answer;
      notify();
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

   Executor m_executor;
   ares_socket_t m_fd; // c-ares' descriptor, which is what the log messages go by
   int m_domain;
   tls_options m_options;
   const log_function* m_logger; // null once stopped
   TcpEndpoint m_server;
   anyhttp::quic::Connection m_connection;
   anyhttp::Event m_stop; // set by stop(), for the connection that is being made
   std::string m_sent; // what c-ares has sent of a message that is not complete yet
   std::vector<std::string> m_pending; // queries waiting for the connection
   std::string m_received; // answers that c-ares has not read yet
   std::function<void(const error_code&)> m_on_readable; // the resolver's wait for reading
   bool m_stopped = false;
};

// --------------------------------------------------------------------------------------------------

inline quic_transport::descriptor::descriptor(std::shared_ptr<bridge> bridge) noexcept
   : m_bridge(std::move(bridge))
{
}

inline void quic_transport::descriptor::detach()
{
   if (m_bridge)
      std::exchange(m_bridge, nullptr)->cancel_wait();
}

inline void quic_transport::descriptor::wait(wait_type type,
                                             std::function<void(const error_code&)> on_ready)
{
   if (m_bridge)
      m_bridge->wait(type, std::move(on_ready));
}

// --------------------------------------------------------------------------------------------------

inline quic_transport::quic_transport(Executor executor, tls_options options,
                                      const log_function& logger)
   : m_executor(std::move(executor)), m_options(std::move(options)), m_logger(logger)
{
}

inline quic_transport::~quic_transport()
{
   for (auto& [fd, bridge] : m_bridges)
      bridge->stop();
}

inline void quic_transport::install(ares_channel_t* channel)
{
   if (const auto status = ares_set_socket_functions_ex(channel, &functions(), this);
       status != ARES_SUCCESS)
      throw std::runtime_error(std::string("ares_set_socket_functions_ex: ") +
                               ares_strerror(static_cast<int>(status)));
}

inline std::optional<quic_transport::descriptor>
quic_transport::find_descriptor(ares_socket_t fd) const
{
   const auto it = m_bridges.find(fd);
   if (it == m_bridges.end())
      return std::nullopt;

   return descriptor(it->second);
}

inline quic_transport::bridge* quic_transport::find(ares_socket_t fd) const
{
   const auto it = m_bridges.find(fd);
   return it == m_bridges.end() ? nullptr : it->second.get();
}

// --------------------------------------------------------------------------------------------------

inline ares_socket_t quic_transport::on_socket(int domain, int type, int protocol)
{
   //
   // With ARES_FLAG_USEVC, c-ares sends no query over UDP. It does make UDP sockets all the same:
   // ares_getaddrinfo() sorts the addresses it has found (RFC 6724) by the source address the
   // system would send to each from, which it learns by connecting a UDP socket to it and asking
   // for its local address -- nothing is sent. Those are passed through, untouched, as the TLS
   // transport does.
   //
   if (type != SOCK_STREAM)
   {
      const int fd = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
      return fd < 0 ? ARES_SOCKET_BAD : fd;
   }

   //
   // A socket of ours is no socket of the system's, but its number has to be one that none of
   // those can have while it is ours: c-ares tells its sockets apart by it, and the UDP ones above
   // are the system's. A descriptor that is open has a number of its own, and nothing is ever done
   // on this one -- c-ares calls the functions below for everything, and the resolver waits on
   // the bridge.
   //
   const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return ARES_SOCKET_BAD;

   //
   // Nothing is to be thrown back into c-ares, which is C.
   //
   try
   {
      m_bridges.emplace(fd, std::make_shared<bridge>(m_executor, fd, domain, m_options, m_logger));
   }
   catch (...)
   {
      ::close(fd);
      errno = ENOMEM;
      return ARES_SOCKET_BAD;
   }

   return fd;
}

inline int quic_transport::on_close(ares_socket_t fd)
{
   if (const auto it = m_bridges.find(fd); it != m_bridges.end())
   {
      it->second->stop();
      m_bridges.erase(it);
   }

   return ::close(fd);
}

//
// A socket of ours has none of the options c-ares sets: what they are for is anyhttp's UDP socket,
// which takes none of them. That turns fast open down as well, which is what c-ares takes ENOSYS
// for, see socket_functions.hpp -- and there is no connection that it could take for one that is
// up too early, as the connect is through at once.
//
inline int quic_transport::on_setsockopt(ares_socket_t fd, ares_socket_opt_t opt, const void* value,
                                         ares_socklen_t size)
{
   if (!find(fd))
      return set_socket_option(fd, opt, value, size);

   errno = ENOSYS;
   return -1;
}

//
// To c-ares, the connect is through at once; the QUIC connection takes its time behind it, and the
// queries wait in the bridge until it is there.
//
inline int quic_transport::on_connect(ares_socket_t fd, const struct sockaddr* address,
                                      ares_socklen_t length)
{
   auto* bridge = find(fd);
   if (!bridge)
      return ::connect(fd, address, length);

   const auto server = make_endpoint(address, length);
   if (!server)
   {
      errno = EINVAL;
      return -1;
   }

   bridge->start(*server);
   return 0;
}

inline ares_ssize_t quic_transport::on_recv(ares_socket_t fd, void* buffer, size_t length)
{
   if (auto* bridge = find(fd))
      return bridge->receive(buffer, length);

   return ::recv(fd, buffer, length, 0);
}

//
// A socket of the system's may be closed by the time c-ares writes to it, and that is to be a
// failed write, not a SIGPIPE.
//
inline ares_ssize_t quic_transport::on_send(ares_socket_t fd, const void* buffer, size_t length,
                                            int flags)
{
   if (auto* bridge = find(fd))
      return bridge->send(buffer, length);

   return ::send(fd, buffer, length, flags | MSG_NOSIGNAL);
}

//
// The local address of a socket of ours is that of the UDP socket that anyhttp has made for the
// connection, which is not known yet when c-ares asks. What it gets is the family it asked for.
//
inline int quic_transport::on_getsockname(ares_socket_t fd, struct sockaddr* address,
                                          ares_socklen_t* length)
{
   auto* bridge = find(fd);
   if (!bridge)
      return ::getsockname(fd, address, length);

   const auto size = bridge->domain() == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
   if (!address || !length || *length < size)
   {
      errno = EINVAL;
      return -1;
   }

   std::memset(address, 0, size);
   address->sa_family = static_cast<sa_family_t>(bridge->domain());
   *length = static_cast<ares_socklen_t>(size);
   return 0;
}

//
// A local address to send from would be one for anyhttp's UDP socket, and the connection does not
// take one, so a socket of ours is not bound at all: a channel with a local address set sends from
// wherever the system routes it.
//
inline int quic_transport::on_bind(ares_socket_t fd, const struct sockaddr* address,
                                   ares_socklen_t length)
{
   if (!find(fd))
      return ::bind(fd, address, length);

   return 0;
}

// --------------------------------------------------------------------------------------------------

//
// The C side of it.
//
inline const struct ares_socket_functions_ex& quic_transport::functions()
{
   static const struct ares_socket_functions_ex table{
      .version = 1,
      .flags = ARES_SOCKFUNC_FLAG_NONBLOCKING,

      .asocket = [](int domain, int type, int protocol, void* data) -> ares_socket_t {
         return static_cast<quic_transport*>(data)->on_socket(domain, type, protocol);
      },

      .aclose = [](ares_socket_t fd, void* data) -> int {
         return static_cast<quic_transport*>(data)->on_close(fd);
      },

      .asetsockopt = [](ares_socket_t fd, ares_socket_opt_t opt, const void* value,
                        ares_socklen_t size, void* data) -> int {
         return static_cast<quic_transport*>(data)->on_setsockopt(fd, opt, value, size);
      },

      .aconnect = [](ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length,
                     unsigned int, void* data) -> int {
         return static_cast<quic_transport*>(data)->on_connect(fd, address, length);
      },

      .arecvfrom = [](ares_socket_t fd, void* buffer, size_t length, int, struct sockaddr* address,
                      ares_socklen_t* address_len, void* data) -> ares_ssize_t {
         if (address && address_len)
            *address_len = 0;
         return static_cast<quic_transport*>(data)->on_recv(fd, buffer, length);
      },

      .asendto = [](ares_socket_t fd, const void* buffer, size_t length, int flags,
                    const struct sockaddr*, ares_socklen_t, void* data) -> ares_ssize_t {
         return static_cast<quic_transport*>(data)->on_send(fd, buffer, length, flags);
      },

      .agetsockname = [](ares_socket_t fd, struct sockaddr* address, ares_socklen_t* length,
                         void* data) -> int { //
         return static_cast<quic_transport*>(data)->on_getsockname(fd, address, length);
      },

      .abind = [](ares_socket_t fd, unsigned int, const struct sockaddr* address,
                  ares_socklen_t length, void* data) -> int { //
         return static_cast<quic_transport*>(data)->on_bind(fd, address, length);
      },

      .aif_nametoindex = [](const char* name, void*) -> unsigned int {
         return ::if_nametoindex(name);
      },

      .aif_indextoname = [](unsigned int index, char* buffer, size_t size, void*) -> const char* {
         if (size < IF_NAMESIZE)
            return nullptr;
         return ::if_indextoname(index, buffer);
      },
   };

   return table;
}

} // namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail

#endif // C_ARES_ASYNC_DOQ
