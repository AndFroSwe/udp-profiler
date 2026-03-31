#include "connection.h"

Connection::~Connection() {
  close();
}

void Connection::close() {
  if (sock != INVALID_SOCKET) {
    closesocket(sock);
  }
  sock = INVALID_SOCKET;
}

bool Connection::init(int socket_timeout_ms) {
  // cant init twice
  if (sock != INVALID_SOCKET) {
    return false;
  }

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock == INVALID_SOCKET) {
    return false;
  }

  // set socket timeout
  const auto timeout_ms = static_cast<DWORD>(socket_timeout_ms); // 1 second
  if (setsockopt(sock,                                           // socket
                 SOL_SOCKET, SO_RCVTIMEO,                        // timeout
                 reinterpret_cast<const char *>(&timeout_ms),    // timeout
                 sizeof(timeout_ms)                              // size
                 ) == SOCKET_ERROR) {
    return false;
  }

  return true;
}

bool Connection::bind_local(const std::string_view ip, const uint16_t port) {
  if (!set_target(local, ip, port)) {
    return false;
  }

  // success, attempt to bind
  if (bind(sock,
           (sockaddr *)&local.value(), // address
           sizeof(local.value())) != 0) {
    local = std::nullopt;
    return false;
  }

  return true;
}

// use recvfrom if has no remote, otherwise just receive
int Connection::receive_on_local_and_save_remote(char *data, size_t bufsize) {
  // must have local to receive
  if (!local) {
    return SOCKET_ERROR;
  }

  int bytes = 0;
  if (!remote.has_value()) {
    int client_len = sizeof(remote.value());
    sockaddr_in new_remote;
    ZeroMemory(&new_remote, sizeof(new_remote));
    bytes = recvfrom(sock,                                      // socket
                     data,                                      // recv buf. reuse send buf
                     static_cast<int>(bufsize),                 // buffer size
                     0,                                         // flags
                     reinterpret_cast<sockaddr *>(&new_remote), // client addr
                     &client_len);                              // length of client addr
    if (bytes != SOCKET_ERROR) {
      remote = new_remote; // save the remote
    }
  } else {
    // just receive, we know the remote
    bytes = recv(sock,                      // socket
                 data,                      // recv buf. reuse send buf
                 static_cast<int>(bufsize), // buffer size
                 0);                        // flags
  }

  return bytes;
}

bool Connection::create_remote(const std::string_view ip, const uint16_t port) {
  return set_target(remote, ip, port);
}

int Connection::send_to_remote(const char *data, const size_t bufsize) {
  // need a remote to send value
  if (!remote) {
    return SOCKET_ERROR;
  }

  return sendto(sock,                                          // socket
                data,                                          // buf
                static_cast<int>(bufsize),                     // buf len
                0,                                             // flags
                reinterpret_cast<sockaddr *>(&remote.value()), // receiver
                sizeof(remote.value()));                       // receiver struct size
}

int Connection::receive_on_local(char *data, const size_t bufsize) const {
  // need local to send
  if (!local) {
    return 0;
  }

  return recv(sock,                      // socket
              data,                      // recv buf. reuse send buf
              static_cast<int>(bufsize), // buffer size
              0);                        // flags
}

void Connection::reset_remote() {
  if (remote) {
    remote = {};
  }
}

std::optional<Connection::ip_addr> Connection::get_remote_info() {
  if (!remote) {
    return {};
  }

  char buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(remote->sin_addr), buf, sizeof(buf));

  return ip_addr{.ip = std::string(buf), .port = remote->sin_port};
}

bool Connection::set_target(std::optional<sockaddr_in> &target, const std::string_view ip, const uint16_t port) {
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
