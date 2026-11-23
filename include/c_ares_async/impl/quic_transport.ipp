// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <c_ares_async/runtime.hpp>

#if C_ARES_ASYNC_DOQ && C_ARES_ASYNC_ASIO // as detail/quic_transport.hpp: empty without

#include <c_ares_async/detail/log.hpp>
#include <c_ares_async/detail/quic_transport.hpp> // NOLINT(misc-header-include-cycle)
#include <c_ares_async/detail/socket_functions.hpp>

#include <anyhttp/quic.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

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
// The coroutines take a shared_ptr to the bridge, which is what keeps it around -- c-ares closing
// its socket only stops it. All of it runs on the resolver's executor, so it needs no lock.
//
class quic_transport::bridge : public std::enable_shared_from_this<bridge>
{
public:
   bridge(const Executor& executor, ares_socket_t fd, int domain, int peer,
          const tls_options& options, const log_function& logger)
      : m_executor(executor), m_fd(fd), m_domain(domain), m_socket(executor),
        m_options(options), m_logger(&logger)
   {
      m_socket.assign(boost::asio::local::stream_protocol(), peer);
   }

   int domain() const noexcept { return m_domain; }

   //
   // Connects to 'server', and passes the queries on once there is a connection.
   //
   void start(const TcpEndpoint& server)
   {
      m_server = server;
      m_connecting = true;
      boost::asio::co_spawn(m_executor, run(shared_from_this()),
                            boost::asio::bind_cancellation_slot(m_cancel.slot(), ignore_exception));
   }

   //
   // Ends it all: the connection that is being made, the QUIC connection with what is open on it,
   // and our end of the socketpair, which is the end of the stream for c-ares. Called by c-ares
   // closing its socket as well as by whatever went wrong here; whichever is first does it.
   //
   // From here on, nothing looks at the resolver's logger any more, which may be gone.
   //
   void stop()
   {
      if (m_stopped)
         return;

      m_stopped = true;
      m_logger = nullptr;
      m_waiting.clear();

      if (m_connecting)
         m_cancel.emit(boost::asio::cancellation_type::terminal);

      //
      // Let go of the connection, rather than just close it. anyhttp keeps the handler of the
      // accept_stream() that watch() waits in, and with that the bridge, until the connection has
      // gone through its closing period -- and the bridge is not to keep the connection in turn,
      // or the two keep each other where the context does not run that long.
      //
      auto connection = std::move(m_connection);
      connection.close(static_cast<std::uint64_t>(doq_error::no_error));

      boost::system::error_code ignored;
      m_socket.close(ignored);
   }

private:
   static void ignore_exception(const std::exception_ptr&) noexcept {}

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

   //
   // The connection, and then the queries, as c-ares writes them to its end of the socketpair.
   //
   static Task<void> run(std::shared_ptr<bridge> self)
   {
      using boost::asio::as_tuple;
      using boost::asio::use_awaitable;

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
      auto [ec, connection] = co_await anyhttp::quic::connect(
         self->m_executor, {self->m_server}, self->m_options.hostname, std::move(config));
      self->m_connecting = false;

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

      boost::asio::co_spawn(self->m_executor, watch(self), ignore_exception);

      for (;;)
      {
         std::array<unsigned char, doq_prefix_size> prefix{};
         auto [read_ec, n] = co_await boost::asio::async_read(
            self->m_socket, boost::asio::buffer(prefix), as_tuple(use_awaitable));
         if (read_ec)
            break; // c-ares has closed its end, or we have closed ours

         const std::size_t size = (std::size_t{prefix[0]} << 8) | prefix[1];
         if (size < dns_header_size)
         {
            self->log(log_level::error, "c-ares sent a message of {} bytes", size);
            break;
         }

         std::string query(doq_prefix_size + size, '\0');
         std::memcpy(query.data(), prefix.data(), prefix.size());
         auto [body_ec, m] = co_await boost::asio::async_read(
            self->m_socket, boost::asio::buffer(query.data() + doq_prefix_size, size),
            as_tuple(use_awaitable));
         if (body_ec)
            break;

         self->start_exchange(std::move(query));
      }

      self->stop();
   }

   //
   // A DoQ server opens no streams of its own (RFC 9250, section 4.2), so waiting for one is
   // waiting for the connection to end -- an idle timeout, the server closing it -- which has to
   // be the end of the socketpair too, or c-ares would go on sending into a connection that is not
   // there any more.
   //
   static Task<void> watch(std::shared_ptr<bridge> self)
   {
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

   //
   // One query on a stream of its own. At the limit of streams that the server allows, it waits
   // for one of those to finish.
   //
   void start_exchange(std::string query)
   {
      if (m_stopped)
         return;

      auto [ec, stream] = m_connection.open_stream();
      if (ec == anyhttp::errors::would_block)
      {
         log(log_level::debug, "waiting for a stream, {} waiting already", m_waiting.size());
         m_waiting.push_back(std::move(query));
         return;
      }

      if (ec)
      {
         log(log_level::error, "cannot open a stream: {}", ec.message());
         stop();
         return;
      }

      boost::asio::co_spawn(m_executor,
                            exchange(shared_from_this(), std::move(stream), std::move(query)),
                            ignore_exception);
   }

   //
   // The ones that had to wait for a stream, for as long as there are streams for them.
   //
   void start_waiting()
   {
      while (!m_waiting.empty() && !m_stopped)
      {
         auto query = std::move(m_waiting.front());
         m_waiting.pop_front();

         const auto waiting = m_waiting.size();
         start_exchange(std::move(query));
         if (m_waiting.size() > waiting)
         {
            // back at the end of the line: put it in front again, it was first
            m_waiting.push_front(std::move(m_waiting.back()));
            m_waiting.pop_back();
            break;
         }
      }
   }

   static Task<void> exchange(std::shared_ptr<bridge> self, anyhttp::quic::Stream stream,
                              std::string query)
   {
      co_await self->ask(stream, std::move(query));
      stream.reset();
      self->start_waiting();
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

      auto [write_ec, written] = co_await stream.write_eof(boost::asio::buffer(query));
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
         auto [read_ec, n] = co_await stream.read_some(boost::asio::buffer(buffer));
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

      answer[doq_prefix_size] = id[0];
      answer[doq_prefix_size + 1] = id[1];
      send_answer(std::move(answer));
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

   //
   // The answers, in the order they came in, to c-ares' end of the socketpair -- one write at a
   // time, which is what keeps them from getting mixed up.
   //
   void send_answer(std::string answer)
   {
      if (m_stopped)
         return;

      m_answers.push_back(std::move(answer));
      if (!m_writing)
      {
         m_writing = true;
         boost::asio::co_spawn(m_executor, write_answers(shared_from_this()), ignore_exception);
      }
   }

   static Task<void> write_answers(std::shared_ptr<bridge> self)
   {
      while (!self->m_answers.empty() && !self->m_stopped)
      {
         auto [ec, n] = co_await boost::asio::async_write(
            self->m_socket, boost::asio::buffer(self->m_answers.front()),
            boost::asio::as_tuple(boost::asio::use_awaitable));
         if (ec)
         {
            self->stop();
            break;
         }
         self->m_answers.pop_front();
      }

      self->m_writing = false;
   }

   Executor m_executor;
   ares_socket_t m_fd; // c-ares' end, which is what the log messages go by
   int m_domain;
   boost::asio::local::stream_protocol::socket m_socket; // our end
   tls_options m_options;
   const log_function* m_logger; // null once stopped
   TcpEndpoint m_server;
   anyhttp::quic::Connection m_connection;
   boost::asio::cancellation_signal m_cancel; // for the connection that is being made
   std::deque<std::string> m_waiting; // queries waiting for a stream
   std::deque<std::string> m_answers; // answers waiting for c-ares
   bool m_connecting = false;
   bool m_writing = false;
   bool m_stopped = false;
};

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

   int fds[2];
   if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0)
      return ARES_SOCKET_BAD;

   //
   // Nothing is to be thrown back into c-ares, which is C. Once there is a bridge, our end is its
   // socket's to close, and only before that ours.
   //
   std::shared_ptr<bridge> created;
   try
   {
      created = std::make_shared<bridge>(m_executor, fds[0], domain, fds[1], m_options, m_logger);
      m_bridges.emplace(fds[0], created);
   }
   catch (...)
   {
      if (!created)
         ::close(fds[1]);
      ::close(fds[0]);
      errno = ENOMEM;
      return ARES_SOCKET_BAD;
   }

   return fds[0];
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
// The socketpair is connected from the start, so to c-ares the connect is through at once; the
// QUIC connection takes its time behind it, and the queries wait in the socketpair until it is
// there.
//
inline int quic_transport::on_connect(ares_socket_t fd, const struct sockaddr* address,
                                      ares_socklen_t length)
{
   const auto it = m_bridges.find(fd);
   if (it == m_bridges.end())
      return ::connect(fd, address, length);

   TcpEndpoint server;
   if (length > server.capacity())
   {
      errno = EINVAL;
      return -1;
   }

   std::memcpy(server.data(), address, length);
   server.resize(length);
   it->second->start(server);
   return 0;
}

//
// The local address of a socket of ours is that of the UDP socket that anyhttp has made for the
// connection, which is not known yet when c-ares asks. What it gets is the family it asked for.
//
inline int quic_transport::on_getsockname(ares_socket_t fd, struct sockaddr* address,
                                          ares_socklen_t* length)
{
   const auto it = m_bridges.find(fd);
   if (it == m_bridges.end())
      return ::getsockname(fd, address, length);

   const auto size = it->second->domain() == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
   if (!address || !length || *length < size)
   {
      errno = EINVAL;
      return -1;
   }

   std::memset(address, 0, size);
   address->sa_family = static_cast<sa_family_t>(it->second->domain());
   *length = static_cast<ares_socklen_t>(size);
   return 0;
}

// --------------------------------------------------------------------------------------------------

//
// The C side of it. Reads and writes are on c-ares' end of the socketpair, as they would be on a
// TCP socket.
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

      //
      // Fast open is turned down, as everywhere else, see socket_functions.hpp. What is left goes
      // to the socketpair, which is where c-ares' end of things is.
      //
      .asetsockopt = [](ares_socket_t fd, ares_socket_opt_t opt, const void* value,
                        ares_socklen_t size, void*) -> int { //
         return set_socket_option(fd, opt, value, size);
      },

      .aconnect = [](ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length,
                     unsigned int, void* data) -> int {
         return static_cast<quic_transport*>(data)->on_connect(fd, address, length);
      },

      .arecvfrom = [](ares_socket_t fd, void* buffer, size_t length, int, struct sockaddr* address,
                      ares_socklen_t* address_len, void*) -> ares_ssize_t {
         if (address && address_len)
            *address_len = 0;
         return ::recv(fd, buffer, length, 0);
      },

      //
      // Our end may be closed by the time c-ares writes, and that is to be a failed write, not a
      // SIGPIPE.
      //
      .asendto = [](ares_socket_t fd, const void* buffer, size_t length, int flags,
                    const struct sockaddr*, ares_socklen_t, void*) -> ares_ssize_t {
         return ::send(fd, buffer, length, flags | MSG_NOSIGNAL);
      },

      .agetsockname = [](ares_socket_t fd, struct sockaddr* address, ares_socklen_t* length,
                         void* data) -> int { //
         return static_cast<quic_transport*>(data)->on_getsockname(fd, address, length);
      },

      //
      // A local address to send from would be one for anyhttp's UDP socket, and anyhttp has no way
      // of being given one. Binding the socketpair to it would fail, so it is not done at all: a
      // channel with a local address set sends from wherever the system routes it.
      //
      .abind = [](ares_socket_t, unsigned int, const struct sockaddr*, ares_socklen_t,
                  void*) -> int { return 0; },

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

#endif // C_ARES_ASYNC_DOQ && C_ARES_ASYNC_ASIO
