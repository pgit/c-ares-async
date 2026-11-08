// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The event loop of Corosio, see ../event_loop.hpp. asio/event_loop.hpp has the documentation of
// each of these, and what is said here is what differs.
//
#pragma once

#include <c_ares_async/runtime.hpp>

#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ip_address.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace c_ares_async::detail
{

enum class wait_type
{
   read,
   write
};

//
// One socket of c-ares', watched but not owned.
//
// Corosio has no descriptor type that would take any file descriptor: readiness comes from the
// sockets, which is where assign() adopts one that was made elsewhere -- and the socket has to be
// of the right kind, so which one it is comes from SO_TYPE. Either way nothing but wait() is ever
// called on it, and release() hands the descriptor back to c-ares' ownership.
//
class Descriptor
{
public:
   Descriptor(const Executor& executor, int fd) : executor_(executor)
   {
      int type = 0;
      socklen_t length = sizeof(type);
      if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0)
         throw system_error(last_error(), "getsockopt(SO_TYPE)");

      auto impl = std::make_shared<state>();
      const auto ec =
         type == SOCK_STREAM
            ? impl->socket.emplace<boost::corosio::tcp_socket>(executor.context()).assign(fd)
            : impl->socket.emplace<boost::corosio::udp_socket>(executor.context()).assign(fd);
      if (ec)
         throw system_error(ec, "assign");

      impl_ = std::move(impl);
   }

   ~Descriptor() { detach(); }

   Descriptor(const Descriptor&) = delete;
   Descriptor& operator=(const Descriptor&) = delete;

   bool is_open() const noexcept { return impl_ != nullptr; }

   //
   // The waits that are still pending hold the socket alive until they have been stopped and
   // their tasks are through, which is why the state lives in a shared_ptr of its own.
   //
   void detach()
   {
      if (!impl_)
         return;

      impl_->stop.request_stop();
      std::visit(
         [](auto& socket) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(socket)>, std::monostate>)
               if (socket.is_open())
                  (void)socket.release();
         },
         impl_->socket);

      impl_.reset();
   }

   //
   // The wait is posted before it is even started: c-ares hands us its sockets from inside
   // ares_process_fds(), through the socket state callback, so a wait armed from there would
   // otherwise be started -- and a readiness that holds already completed, and its callback
   // called -- on the stack of that very call. c-ares does not survive being re-entered like
   // that: it closes its connections underneath itself and comes apart.
   //
   // Corosio answers "is this socket ready" with a poll() of its own before it parks anything,
   // which is what makes this the common case rather than a rare one, where Asio only ever
   // completes a wait from its own event loop.
   //
   template <typename F>
   void wait(wait_type type, F&& on_ready)
   {
      auto wait = [](std::shared_ptr<state> impl, wait_type type,
                     std::decay_t<F> on_ready) -> Task<void> {
         co_await yield_now();
         on_ready(co_await impl->wait(type));
      };
      boost::capy::run_async(
         executor_, impl_->stop.get_token(), []() noexcept {},
         [](const std::exception_ptr&) noexcept {})(wait(impl_, type, std::forward<F>(on_ready)));
   }

private:
   struct state
   {
      //
      // The two socket types have awaitables of their own, so this is a coroutine picking one of
      // them rather than a visit, which would have to come back with a common type.
      //
      Task<error_code> wait(wait_type type)
      {
         const auto what = type == wait_type::read ? boost::corosio::wait_type::read
                                                   : boost::corosio::wait_type::write;

         error_code ec;
         if (auto* tcp = std::get_if<boost::corosio::tcp_socket>(&socket))
            std::tie(ec) = co_await tcp->wait(what);
         else if (auto* udp = std::get_if<boost::corosio::udp_socket>(&socket))
            std::tie(ec) = co_await udp->wait(what);
         else
            co_return errors::canceled; // released underneath us

         //
         // A socket that has failed is ready as far as a wait is concerned: whoever owns it --
         // c-ares, or the TLS session underneath it -- is the one to find the failure out, by
         // reading or writing, and to fail or retry its query over it. Corosio reports the error
         // of the socket as the error of the wait, where Asio reports such a wait as ready,
         // so this is where the two are made to say the same thing.
         //
         if (ec && ec != boost::capy::cond::canceled && ec != std::errc::bad_file_descriptor)
            ec = {};

         co_return ec;
      }

      std::variant<std::monostate, boost::corosio::tcp_socket, boost::corosio::udp_socket> socket;
      std::stop_source stop;
   };

   Executor executor_;
   std::shared_ptr<state> impl_;
};

inline std::string address_text(const std::array<unsigned char, 4>& bytes)
{
   return boost::corosio::ipv4_address(bytes).to_string();
}

inline std::string address_text(const std::array<unsigned char, 16>& bytes)
{
   return boost::corosio::ipv6_address(bytes).to_string();
}

inline std::optional<TcpEndpoint> make_endpoint(const sockaddr* address, std::size_t length)
{
   if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in))
   {
      sockaddr_in sa{};
      std::memcpy(&sa, address, sizeof(sa));

      boost::corosio::ipv4_address::bytes_type bytes;
      std::memcpy(bytes.data(), &sa.sin_addr, bytes.size());
      return TcpEndpoint(boost::corosio::ipv4_address(bytes), ntohs(sa.sin_port));
   }

   if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6))
   {
      sockaddr_in6 sa{};
      std::memcpy(&sa, address, sizeof(sa));

      boost::corosio::ipv6_address::bytes_type bytes;
      std::memcpy(bytes.data(), &sa.sin6_addr, bytes.size());
      return TcpEndpoint(boost::corosio::ipv6_address(bytes, sa.sin6_scope_id),
                         ntohs(sa.sin6_port));
   }

   return std::nullopt;
}

} // namespace c_ares_async::detail
