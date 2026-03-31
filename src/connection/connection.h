#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class Connection {
public:
  struct ip_addr {
    std::string ip;
    uint16_t port;
  };

  Connection() noexcept;
  ~Connection() noexcept;

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
  struct Impl; // OS specific information
  std::unique_ptr<Impl> m_impl;
};
