#include "connection.h"

#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>

#include <cassert>
#include <iostream>

namespace {
// helper for setting local or remote
bool set_target(std::optional<sockaddr_in> &target, std::string_view ip, uint16_t port);

// windows sockets wsa raii helper
struct WSARAII {
  WSAData wsa;
  bool is_init;

  WSARAII() : is_init(false) {
  }
  ~WSARAII() {
    if (is_init) {
      WSACleanup();
    }
  }

  bool init() {
    // cant initialize twice
    if (is_init) {
      return false;
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      return false;
    }
    is_init = true;
    return true;
  }
};

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
  std::optional<WSARAII> wsa;
};

Connection::Connection() noexcept {
  m_impl = std::make_unique<Impl>(); // create the pimpl
}

Connection::~Connection() noexcept {
  close();
}

void Connection::close() {
  if (m_impl->sock != INVALID_SOCKET) {
    closesocket(m_impl->sock);
  }
  m_impl->sock = INVALID_SOCKET;
}

bool Connection::init(int socket_timeout_ms) {
  // only init once
  if (!m_impl->wsa) {
    m_impl->wsa = WSARAII();
    if (!m_impl->wsa->init()) {
      m_impl = {};
      return false;
    }
  }

  // cant init twice
  if (m_impl->sock != INVALID_SOCKET) {
    return false;
  }

  m_impl->sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (m_impl->sock == INVALID_SOCKET) {
    return false;
  }

  // set socket timeout
  const auto timeout_ms = static_cast<DWORD>(socket_timeout_ms); // 1 second
  if (setsockopt(m_impl->sock,                                   // socket
                 SOL_SOCKET, SO_RCVTIMEO,                        // timeout
                 reinterpret_cast<const char *>(&timeout_ms),    // timeout
                 sizeof(timeout_ms)                              // size
                 ) == SOCKET_ERROR) {
    closesocket(m_impl->sock);
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

  int bytes = 0;
  if (!m_impl->remote.has_value()) {
    int client_len = sizeof(m_impl->remote.value());
    sockaddr_in new_remote;
    ZeroMemory(&new_remote, sizeof(new_remote));
    bytes = recvfrom(m_impl->sock,                              // socket
                     reinterpret_cast<char *>(buf.data()),      // recv buf. reuse send buf
                     static_cast<int>(buf.size()),              // buffer size
                     0,                                         // flags
                     reinterpret_cast<sockaddr *>(&new_remote), // client addr
                     &client_len);                              // length of client addr
    if (bytes != SOCKET_ERROR) {
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
    const auto err = WSAGetLastError(); // get the error
    switch (err) {
    case WSAETIMEDOUT:
      return make_error(ReturnCode::TIMEOUT);
      break;
    case WSAECONNRESET:
      return make_error(ReturnCode::ICMP);
      break;
    case WSAEMSGSIZE:
      return make_error(ReturnCode::WRONG_SIZE);
      break;
    default:
      assert(false && "unknown error on receive_on_local_and_save_remote");
      break;
    }
  }

  return make_ok(bytes);
}

bool Connection::create_remote(const std::string_view ip, const uint16_t port) {
  return set_target(m_impl->remote, ip, port);
}
TranscieveResult Connection::send_to_remote(const std::vector<std::byte> &buf, std::optional<size_t> bufsize) {
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

  if (res != buf.size()) {
    return make_error(ReturnCode::SEND_ERROR);
  }

  return make_ok(res);
}

TranscieveResult Connection::receive_on_local(std::vector<std::byte> &buf) const {
  // need local to send
  if (!m_impl->local) {
    return make_error(ReturnCode::NO_LOCAL);
  }

  int bytes = recv(m_impl->sock,                         // m_impl->socket
                   reinterpret_cast<char *>(buf.data()), // buf
                   static_cast<int>(buf.size()),         // buf len
                   0);                                   // flags

  // handle errors
  if (bytes < 0) {
    const auto err = WSAGetLastError(); // get the error
    switch (err) {
    case WSAETIMEDOUT:
      return make_error(ReturnCode::TIMEOUT);
      break;
    case WSAECONNRESET:
      return make_error(ReturnCode::ICMP);
      break;
    case WSAEMSGSIZE:
      return make_error(ReturnCode::WRONG_SIZE);
      break;
    default:
      assert(false && "unknown error on receive_on_local");
      break;
    }
  }

  return make_ok(bytes);
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
  ZeroMemory(&t, sizeof(t));
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
