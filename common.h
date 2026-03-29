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
#include <cstdlib>
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

  // use recvfrom if has no remote, otherwise just receive
  int receive_on_local_and_save_remote(char *data, size_t bufsize) {
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

  bool create_remote(const std::string_view ip, const uint16_t port) {
    return set_target(remote, ip, port);
  }

  int send_to_remote(const char *data, const size_t bufsize) {
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

  int receive_on_local(char *data, const size_t bufsize) const {
    // need local to send
    if (!local) {
      return 0;
    }

    return recv(sock,                      // socket
                data,                      // recv buf. reuse send buf
                static_cast<int>(bufsize), // buffer size
                0);                        // flags
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
  uint32_t message_id;
  uint32_t message_size;
  int64_t server_send_timestamp_ns;
  int64_t client_receive_timestamp_ns;
  int64_t client_send_timestamp_ns;
  int64_t server_receive_timestamp_ns;
};

// measurement data
struct Measurement {
  uint32_t sends;
  uint32_t errors;
  std::vector<int64_t> server_send_timestamp_ns;
  std::vector<int64_t> client_receive_timestamp_ns;
  std::vector<int64_t> client_send_timestamp_ns;
  std::vector<int64_t> server_receive_timestamp_ns;
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

[[nodiscard]] inline int64_t get_steady_timestamp_ns() {
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

[[nodiscard]] inline KPIs calculate_kpis(std::vector<int64_t> &t0, std::vector<int64_t> &t1, int64_t offset) {
  assert(!t0.empty() && !t1.empty() && "cant get results of empty vector");
  const size_t n = t0.size();
  assert(t0.size() == t1.size() && "kpi vectors not equal length");
  assert(t0[0] <= t1[0] && "t0 times should be before t1");

  std::vector<int64_t> res(n); // results vector
  // calculate the diffs
  for (size_t i = 0; i < n; i++) {
    res[i] = t1[i] - t0[i] + offset;
  }

  std::ranges::sort(res); // need to sort for median

  // use double to avoid overflow
  double total = std::accumulate(res.begin(), res.end(), 0.0); // sum up
  double mean = total / n;                                     // get the mean

  // calculate standard deviation
  // use the loop to get min and max as well
  int64_t max_val = 0;
  int64_t min_val = 0;
  double sq_sum = 0.0;
  for (const int64_t v : res) {
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
  kpi.median = (n % 2 == 0) ? (res[n / 2] + res[(n / 2) - 1]) / 2.0 // even: avg two middles NOLINT(readability-magic*)
                            : res[n / 2];                           // odd: middle element
  kpi.stddev = std::sqrt(sq_sum / divisor);
  kpi.max_val = max_val;
  kpi.min_val = min_val;
  kpi.p95 = percentile(res, 0.95); // NOLINT(readability-magic*)

  return kpi;
}

[[nodiscard]] inline bool is_localhost(const std::string_view addr) {
  return addr == "127.0.0.1";
}

// use cristians algorithm to calculate clock offset
[[nodiscard]] inline int64_t calculate_clock_offset(const Measurement &m) {
  assert(!m.server_send_timestamp_ns.empty() && !m.client_receive_timestamp_ns.empty() &&
         !m.client_send_timestamp_ns.empty() && !m.server_receive_timestamp_ns.empty() && "invalid measurements");

  const size_t n = m.server_receive_timestamp_ns.size();

  assert(m.server_send_timestamp_ns.size() == n && m.client_receive_timestamp_ns.size() == n &&
         m.client_send_timestamp_ns.size() == n && m.server_receive_timestamp_ns.size() == n &&
         "measurements not equal length");

  std::vector<double> offsets(n);
  for (size_t i = 0; i < n; i++) {
    offsets[i] = ((m.client_receive_timestamp_ns[i] - m.server_send_timestamp_ns[i]) -
                  (m.server_receive_timestamp_ns[i] - m.client_send_timestamp_ns[i])) /
                 2.0; // NOLINT(readability-magic*)
  }

  // return median offset
  std::sort(offsets.begin(), offsets.end());
  return (n % 2 == 0) ? (offsets[n / 2] + offsets[(n / 2) - 1]) / 2.0 : offsets[n / 2]; // NOLINT(readability-magic*)
}

// common parameters
constexpr size_t MIN_MESSAGE_SIZE = sizeof(Payload); // need to send at least payload
constexpr size_t MAX_MESSAGE_SIZE = 4096;            // max allowable message size
