#pragma once

#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <optional>
#include <string>

class Connection {
public:
  struct ip_addr {
    std::string ip;
    uint16_t port;
  };

  SOCKET sock = INVALID_SOCKET;
  std::optional<sockaddr_in> local;
  std::optional<sockaddr_in> remote;

  Connection() = default;
  ~Connection();

  void close();

  bool init(int socket_timeout_ms);
  bool bind_local(std::string_view ip, uint16_t port);
  bool create_remote(std::string_view ip, uint16_t port);

  // use recvfrom if has no remote, otherwise just receive
  int send_to_remote(const char *data, size_t bufsize);
  int receive_on_local(char *data, size_t bufsize) const;
  int receive_on_local_and_save_remote(char *data, size_t bufsize);

  void reset_remote();
  std::optional<ip_addr> get_remote_info();

private:
  // helper function for setting remote or local
  static bool set_target(std::optional<sockaddr_in> &target, std::string_view ip, uint16_t port);
};
