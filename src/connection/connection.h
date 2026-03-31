#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class ReturnCode : uint8_t {
  OK,                // no error
  WSA_ERROR,         // could not init WSA
  SOCKET_INIT_ERROR, // could not init socket
  SEND_ERROR,        // general send error
  RECEIVE_ERROR,     // general receive error
  NO_REMOTE,         // no remote to send to
  NO_LOCAL,          // no local to receive on
  TIMEOUT,           // timeout on receive
  ICMP,              // ICMP error, no receipient
  WRONG_SIZE,        // did not get/send expected size
};

struct TranscieveResult {
  int bytes;
  ReturnCode ret;
};

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
  TranscieveResult send_to_remote(const std::vector<std::byte> &buf, std::optional<size_t> bufsize = {});
  TranscieveResult receive_on_local(std::vector<std::byte> &buf) const;
  TranscieveResult receive_on_local_and_save_remote(std::vector<std::byte> &buf);

  void reset_remote();
  std::optional<ip_addr> get_remote_info();

private:
  struct Impl; // OS specific information
  std::unique_ptr<Impl> m_impl;
};
