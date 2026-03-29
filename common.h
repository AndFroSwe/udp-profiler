#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <Windows.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numeric>

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
    return sock != INVALID_SOCKET;
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
  static bool set_target(std::optional<sockaddr_in> &target, const std::string_view ip, const uint16_t port) {
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
  int64_t server_send_timestamp_ns;
  int64_t client_receive_timestamp_ns;
  int64_t server_receive_timestamp_ns;
  int64_t message_id;
};

// measurement data
struct Measurement {
  uint32_t sends;
  uint32_t errors;
  std::vector<int64_t> rtt;              // round trip measurements
  std::vector<int64_t> client_to_server; // time from client receive to server response receive
};

struct KPIs {
  int64_t mean;
  int64_t median;
  int64_t stddev;
  int64_t max_val;
  int64_t min_val;
  int64_t p95;
};

// ema calculation
template <typename T> [[nodiscard]] constexpr T update_ema(T old_ema, T new_value) {
  constexpr int SAMPLES = 20; // to calculate EMA ratio
  constexpr double WEIGHT = 1.0 / static_cast<double>(SAMPLES - 1);

  return (old_ema * (1 - WEIGHT)) + (new_value * WEIGHT);
}

[[nodiscard]] inline int64_t get_timestamp_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] inline int64_t percentile(const std::vector<int64_t> &m, double p) {
  // m must already be sorted
  double idx = p * (m.size() - 1);
  size_t lo = static_cast<size_t>(idx);
  size_t hi = lo + 1;
  if (hi >= m.size()) {
    return m.back();
  }
  double frac = idx - lo;
  return m[lo] + (frac * (m[hi] - m[lo])); // linear interpolation
}

[[nodiscard]] inline KPIs calculate_kpis(std::vector<int64_t> &m) {
  assert(!m.empty() && "cant get results of empty vector");

  std::ranges::sort(m); // need to sort for median
  const size_t n = m.size();

  // use double to avoid overflow
  double total = std::accumulate(m.begin(), m.end(), 0.0); // sum up
  double mean = total / n;                                 // get the mean

  // calculate standard deviation
  // use the loop to get min and max as well
  int64_t max_val = 0;
  int64_t min_val = 0;
  double sq_sum = 0.0;
  for (const int64_t v : m) {
    double diff = v - mean;
    sq_sum += diff * diff;

    // min max
    max_val = std::max(v, max_val);
    min_val = min_val == 0 ? v : std::min(v, min_val);
  }
  const size_t divisor = n - 1; // for stddev

  // save the kpis
  KPIs kpi;
  kpi.mean = mean;
  kpi.median = (n % 2 == 0) ? (m[n / 2] + m[(n / 2) - 1]) / 2.0 // even: avg two middles NOLINT(readability-magic*)
                            : m[n / 2];                         // odd: middle element
  kpi.stddev = std::sqrt(sq_sum / divisor);
  kpi.max_val = max_val;
  kpi.min_val = min_val;
  kpi.p95 = percentile(m, 0.95); // NOLINT(readability-magic*)

  return kpi;
}

// common parameters
constexpr size_t MIN_MESSAGE_SIZE = sizeof(Payload); // need to send at least payload
constexpr size_t MAX_MESSAGE_SIZE = 4096;            // max allowable message size
