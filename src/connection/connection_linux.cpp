#include "connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <unistd.h>

using SOCKET = int; // socket file descriptor type

constexpr SOCKET INVALID_SOCKET = -1; // posix sockets invalid socket

namespace {
// helper for setting local or remote
bool set_target(std::optional<sockaddr_in> &target, std::string_view ip, uint16_t port);

// helper for creating a successful return send/receive
constexpr TranscieveResult make_ok(int bytes) {
  assert(bytes > 0 && "less than 1 byte in make_ok");

  return {
      .bytes = bytes,
      .ret = ReturnCode::OK,
  };
}

// helper for making error on send/receive
constexpr TranscieveResult make_error(ReturnCode ret) {
  assert(ret != ReturnCode::OK && "OK in make_error makes no sense");

  return {
      .bytes = 0,
      .ret = ret,
  };
}
} // namespace

struct Connection::Impl {
  SOCKET sock = INVALID_SOCKET;
  std::optional<sockaddr_in> local;
  std::optional<sockaddr_in> remote;
};

Connection::Connection() noexcept {
  m_impl = std::make_unique<Impl>(); // create the pimpl
}

Connection::Connection(Connection &&other) noexcept : m_impl(std::move(other.m_impl)) {
}

Connection &Connection::operator=(Connection &&other) noexcept {
  if (this != &other) {
    close();                          // release own socket first
    m_impl = std::move(other.m_impl); // steal others socket
  }

  return *this;
}

Connection::~Connection() noexcept {
  close();
}

void Connection::close() {
  // must have m_impl to close
  if (m_impl == nullptr) {
    return;
  }

  if (m_impl->sock != INVALID_SOCKET) {
    ::close(m_impl->sock);
  }
  m_impl->sock = INVALID_SOCKET;
}

bool Connection::init(int socket_timeout_ms) {
  // cant init twice
  if (m_impl->sock != INVALID_SOCKET) {
    return false;
  }

  m_impl->sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (m_impl->sock == INVALID_SOCKET) {
    return false;
  }

  // set socket timeout
  timeval tv{
      .tv_sec = socket_timeout_ms / 1000,          // NOLINT(readability-magic-*)
      .tv_usec = (socket_timeout_ms % 1000) * 1000 // NOLINT(readability-magic-*)
  };
  if (setsockopt(m_impl->sock,            // socket
                 SOL_SOCKET, SO_RCVTIMEO, // flags
                 &tv, sizeof(tv)          // timeout data
                 ) < 0) {
    ::close(m_impl->sock);
    return false;
  }

  return true;
}

bool Connection::bind_local(const std::string_view ip, const uint16_t port) {
  if (!set_target(m_impl->local, ip, port)) {
    return false;
  }

  // success, attempt to bind
  if (bind(m_impl->sock,
           (sockaddr *)&m_impl->local.value(), // address
           sizeof(m_impl->local.value())) != 0) {
    m_impl->local = std::nullopt;
    return false;
  }

  return true;
}

// use recvfrom if has no remote, otherwise just receive
TranscieveResult Connection::receive_on_local_and_save_remote(std::vector<std::byte> &buf) {
  // must have local to receive
  if (!m_impl->local) {
    return make_error(ReturnCode::NO_LOCAL);
  }

  ssize_t bytes = 0;
  if (!m_impl->remote.has_value()) {
    socklen_t client_len = sizeof(sockaddr_in);
    sockaddr_in new_remote;
    memset(&new_remote, 0, sizeof(new_remote));
    bytes = recvfrom(m_impl->sock,                              // socket
                     reinterpret_cast<char *>(buf.data()),      // recv buf. reuse send buf
                     static_cast<int>(buf.size()),              // buffer size
                     0,                                         // flags
                     reinterpret_cast<sockaddr *>(&new_remote), // client addr
                     &client_len);                              // length of client addr
    if (bytes >= 0) {
      m_impl->remote = new_remote; // save the remote
    }
  } else {
    // just receive, we know the remote
    bytes = recv(m_impl->sock,                         // m_impl->socket
                 reinterpret_cast<char *>(buf.data()), // recv buf. reuse send buf
                 static_cast<int>(buf.size()),         // buffer size
                 0);                                   // flags
  }

  // handle errors
  if (bytes < 0) {
    const auto err = errno; // cache errno
    switch (err) {
    case EAGAIN: // linux timeout
      return make_error(ReturnCode::TIMEOUT);
    case EINTR: // interrupt sent
      return make_error(ReturnCode::INTERRUPTED);
    default:
      return make_error(ReturnCode::RECEIVE_ERROR);
    }
  }

  return make_ok(static_cast<int>(bytes));
}

bool Connection::create_remote(const std::string_view ip, const uint16_t port) {
  return set_target(m_impl->remote, ip, port);
}

TranscieveResult Connection::send(const std::vector<std::byte> &buf, std::optional<size_t> bufsize) {
  // need a remote to send value
  if (!m_impl->remote) {
    return make_error(ReturnCode::NO_REMOTE);
  }

  int res = sendto(m_impl->sock,                               // m_impl->socket
                   reinterpret_cast<const char *>(buf.data()), // buf
                   bufsize.has_value() ?                       // if given a size by callee
                       static_cast<int>(*bufsize)
                                       :                                  // use that
                       static_cast<int>(buf.size()),                      // otherwise entire buffer
                   0,                                                     // flags
                   reinterpret_cast<sockaddr *>(&m_impl->remote.value()), // receiver
                   sizeof(m_impl->remote.value()));                       // receiver struct size

  if (res != bufsize.value_or(buf.size())) {
    return make_error(ReturnCode::SEND_ERROR);
  }

  // get local ephemereal port if we dont already have a local port
  if (!m_impl->local) {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (getsockname(m_impl->sock, reinterpret_cast<sockaddr *>(&local), &len) == 0) {
      m_impl->local = local;
    }
  }

  return make_ok(res);
}

TranscieveResult Connection::receive(std::vector<std::byte> &buf) const {
  // need local to send
  if (!m_impl->local) {
    return make_error(ReturnCode::NO_LOCAL);
  }

  ssize_t bytes = recv(m_impl->sock,                         // m_impl->socket
                       reinterpret_cast<char *>(buf.data()), // buf
                       static_cast<int>(buf.size()),         // buf len
                       0);                                   // flags

  // handle errors
  if (bytes < 0) {
    const auto err = errno; // cache errno
    switch (err) {
    case EAGAIN: // linux timeout
      return make_error(ReturnCode::TIMEOUT);
    case EINTR: // interrupt sent
      return make_error(ReturnCode::INTERRUPTED);
    default:
      return make_error(ReturnCode::RECEIVE_ERROR);
    }
  }

  return make_ok(static_cast<int>(bytes));
}

void Connection::reset_remote() {
  if (m_impl->remote) {
    m_impl->remote = {};
  }
}

std::optional<Connection::ip_addr> Connection::get_remote_info() {
  if (!m_impl->remote) {
    return {};
  }

  char buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(m_impl->remote->sin_addr), buf, sizeof(buf));

  return ip_addr{.ip = std::string(buf), .port = m_impl->remote->sin_port};
}

namespace {
bool set_target(std::optional<sockaddr_in> &target, const std::string_view ip, const uint16_t port) {
  sockaddr_in t;
  memset(&t, 0, sizeof(t));
  t.sin_family = AF_INET;
  t.sin_port = htons(port);

  // returns 1 on success, 0 on incorrect ip, -1 on other errors
  if (inet_pton(AF_INET, ip.data(), &t.sin_addr) != 1) {
    target = std::nullopt;
    return false;
  }

  // success, save value
  target = t;
  return true;
}
} // namespace
