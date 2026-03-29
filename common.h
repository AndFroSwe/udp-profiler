#pragma once

#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <Windows.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdint>

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

// socket raii helper class
class Connection {
public:
  SOCKET sock = INVALID_SOCKET;
  std::optional<sockaddr_in> local;
  std::optional<sockaddr_in> remote;

  Connection() = default;

  ~Connection() {
    if (sock != INVALID_SOCKET) {
      closesocket(sock);
    }
  }

  bool init() {
    // cant init twice
    if (sock != INVALID_SOCKET) {
      return false;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
      return false;
    }
    return true;
  }

  bool bind_local(const std::string_view ip, const uint16_t port) {
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

  bool set_remote(const std::string_view ip, const uint16_t port) {
    return set_target(remote, ip, port);
  }

private:
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
};

// message payload information
struct Payload {
  uint32_t message_size;
  int64_t send_timestamp_ns;
  int64_t receive_timestamp_ns;
  int64_t send_timestamp2_ns;
  int64_t receive_timestamp2_ns;
  int64_t message_id;
};

// measurement data
struct Measurement {
  uint32_t sends;
  uint32_t errors;
  int64_t ema;
  int64_t min_rtt;
  int64_t max_rtt;
};

// ema calculation
template <typename T> [[nodiscard]] constexpr T update_ema(T old_ema, T new_value) {
  constexpr int SAMPLES = 20; // to calculate EMA ratio
  constexpr double WEIGHT = 1.0 / static_cast<double>(SAMPLES - 1);

  return old_ema * (1 - WEIGHT) + new_value * WEIGHT;
}

[[nodiscard]] inline int64_t get_timestamp_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// common parameters
constexpr size_t MIN_MESSAGE_SIZE = sizeof(Payload); // need to send at least payload
constexpr size_t MAX_MESSAGE_SIZE = 4096;            // max allowable message size
