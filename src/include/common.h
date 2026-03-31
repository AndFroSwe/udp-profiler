#pragma once

#ifndef UNICODE
#define UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <Windows.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

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

template <typename Fn, typename T>
concept BinaryFn = requires(Fn f, T lhs, T rhs) {
  { f(lhs, rhs) } -> std::convertible_to<T>;
};

template <typename T, BinaryFn<T> Fn>
[[nodiscard]] std::vector<T> elementwise(const std::vector<T> &lhs, const std::vector<T> &rhs, Fn fn) {
  assert(lhs.size() == rhs.size() && "elementwise on unequal lengths");

  const size_t n = lhs.size();
  std::vector<T> res(n);

  for (size_t i = 0; i < n; i++) {
    res[i] = fn(lhs[i], rhs[i]);
  }

  return res;
}

[[nodiscard]] inline KPIs calculate_kpis(const std::vector<int64_t> &v) {
  const size_t n = v.size();

  std::vector<int64_t> res(v); // copy to maodify
  std::ranges::sort(res);      // need to sort for median

  // use double to avoid overflow
  double total = std::accumulate(res.begin(), res.end(), 0.0); // sum up
  double mean = total / n;                                     // get the mean

  // calculate standard deviation
  // use the loop to get min and max as well
  int64_t max_val = 0;
  int64_t min_val = 0;
  double sq_sum = 0.0;
  for (const int64_t val : res) {
    double diff = val - mean;
    sq_sum += diff * diff;

    // min max
    max_val = std::max(val, max_val);
    min_val = min_val == 0 ? val : std::min(val, min_val);
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

// common parameters
constexpr size_t MIN_MESSAGE_SIZE = sizeof(Payload);                            // need to send at least payload
constexpr size_t MAX_MESSAGE_SIZE = 4096;                                       // max allowable message size
const std::array<char, 8> SPINNER = {'-', '\\', '|', '/', '-', '\\', '|', '/'}; // spinner glyphs
