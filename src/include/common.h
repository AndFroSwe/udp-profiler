/**
 * @file common.h
 * @brief Shared types, constants, and utility functions used across client and server.
 */

#pragma once

#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <vector>

/**
 * @brief Wire-format message payload exchanged between client and server.
 *
 * Every UDP datagram carries one @c Payload at its start. The four timestamps
 * form a complete two-way latency sample: the server stamps the outgoing
 * message, the client stamps receipt and re-transmission, and the server stamps
 * final arrival. All timestamps are nanoseconds from an arbitrary epoch
 * produced by @c get_steady_timestamp_ns().
 */
struct Payload {
  uint32_t message_id;                 ///< Monotonically increasing sequence number.
  uint32_t message_size;               ///< Total datagram size in bytes, including this header.
  int64_t server_send_timestamp_ns;    ///< When the server sent the message (ns).
  int64_t client_receive_timestamp_ns; ///< When the client received the message (ns).
  int64_t client_send_timestamp_ns;    ///< When the client echoed the message back (ns).
  int64_t server_receive_timestamp_ns; ///< When the server received the echo (ns).
};

/**
 * @brief Accumulates raw timestamp vectors over a measurement run.
 *
 * Each successful round-trip appends one entry to every timestamp vector.
 * Failed sends/receives increment @c errors instead.  All four vectors are
 * always the same length after a completed run.
 */
struct Measurement {
  uint32_t sends;                                   ///< Total send attempts made.
  uint32_t errors;                                  ///< Number of failed send or receive operations.
  std::vector<int64_t> server_send_timestamp_ns;    ///< Per-message server send timestamps (ns).
  std::vector<int64_t> client_receive_timestamp_ns; ///< Per-message client receive timestamps (ns).
  std::vector<int64_t> client_send_timestamp_ns;    ///< Per-message client send timestamps (ns).
  std::vector<int64_t> server_receive_timestamp_ns; ///< Per-message server receive timestamps (ns).
};

/**
 * @brief Descriptive statistics computed from a latency sample vector.
 *
 * All fields are in the same unit as the input (typically nanoseconds).
 * Produced by @c calculate_kpis().
 */
struct KPIs {
  int64_t mean;    ///< Arithmetic mean.
  int64_t median;  ///< 50th percentile (exact for odd @e n, averaged for even @e n).
  int64_t stddev;  ///< Sample standard deviation (denominator @c n-1).
  int64_t max_val; ///< Maximum observed value.
  int64_t min_val; ///< Minimum observed value.
  int64_t p95;     ///< 95th percentile via linear interpolation.
};

/**
 * @brief Updates an Exponential Moving Average (EMA) with a new sample.
 *
 * Uses a fixed window of 20 samples to derive the smoothing weight
 *
 * @tparam T  Arithmetic type of the EMA (e.g. @c double or @c int64_t).
 * @param old_ema    The current EMA value before this update.
 * @param new_value  The latest sample to incorporate.
 * @return The updated EMA value.
 */
template <typename T> [[nodiscard]] constexpr T update_ema(T old_ema, T new_value) {
  constexpr int SAMPLES = 20;
  constexpr double WEIGHT = 1.0 / static_cast<double>(SAMPLES - 1);
  return (old_ema * (1 - WEIGHT)) + (new_value * WEIGHT);
}

/**
 * @brief Returns the current value of the steady clock in nanoseconds.
 *
 * Uses @c std::chrono::steady_clock, which is monotonic and suitable for
 * measuring elapsed time.  The epoch is arbitrary and consistent only
 * within a single process run.
 *
 * @return Nanoseconds since the steady clock's epoch as @c int64_t.
 */
[[nodiscard]] inline int64_t get_steady_timestamp_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/**
 * @brief Computes a percentile value from a sorted sample vector.
 *
 * Uses linear interpolation between adjacent elements when the requested
 * percentile falls between two indices.
 *
 * @pre @p m must be sorted in ascending order.
 * @param m  Sorted sample vector.
 * @param p  Percentile in the range [0.0, 1.0] (e.g. @c 0.95 for P95).
 * @return   Interpolated value at the requested percentile.
 */
[[nodiscard]] inline int64_t percentile(const std::vector<int64_t> &m, double p) {
  assert(p > 0.0 && p < 1.0 && "percentile outsize range [0.0, 1.0]");
  assert(!m.empty() && "empty vector to take percentile from");

  double idx = p * (m.size() - 1);
  size_t lo = static_cast<size_t>(idx);
  size_t hi = lo + 1;
  if (hi >= m.size()) {
    return m.back();
  }
  double frac = idx - lo;
  return m[lo] + (frac * (m[hi] - m[lo]));
}

/**
 * @brief Concept satisfied by a callable that combines two @c T values into one @c T.
 * Helper for elementwise vector calculations
 *
 * @tparam Fn  Callable type to check.
 * @tparam T   Value type on which @p Fn operates.
 */
template <typename Fn, typename T>
concept BinaryFn = requires(Fn f, T lhs, T rhs) {
  { f(lhs, rhs) } -> std::convertible_to<T>;
};

/**
 * @brief Applies a binary function element-wise to two equal-length vectors.
 *
 * @tparam T   Element type.
 * @tparam Fn  Binary callable satisfying @c BinaryFn<T>.
 * @param lhs  Left-hand operand vector.
 * @param rhs  Right-hand operand vector; must be the same length as @p lhs.
 * @param fn   Function applied to each pair of elements.
 * @return A new vector where @c result[i] = fn(lhs[i], rhs[i]).
 *
 * @pre @c lhs.size() == @c rhs.size()
 */
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

/**
 * @brief Computes descriptive statistics for a latency sample vector.
 *
 * Internally sorts a copy of @p v to compute the median and percentile.
 * Standard deviation uses the sample formula (denominator @c n-1).
 * Intermediate sums are accumulated in @c double to avoid integer overflow
 * on large nanosecond values.
 *
 * @param v  Raw sample vector (need not be sorted; must be non-empty).
 * @return   A populated @c KPIs struct
 * */
[[nodiscard]] inline KPIs calculate_kpis(const std::vector<int64_t> &v) {
  const size_t n = v.size();
  std::vector<int64_t> res(v);
  std::ranges::sort(res);

  double total = std::accumulate(res.begin(), res.end(), 0.0);
  double mean = total / n;

  int64_t max_val = 0;
  int64_t min_val = 0;
  double sq_sum = 0.0;
  for (const int64_t val : res) {
    double diff = val - mean;
    sq_sum += diff * diff;
    max_val = std::max(val, max_val);
    min_val = min_val == 0 ? val : std::min(val, min_val);
  }

  KPIs kpi;
  kpi.mean = mean;
  kpi.median = (n % 2 == 0) ?                            // if even
                   (res[n / 2] + res[(n / 2) - 1]) / 2.0 // NOLINT(readability-magic-*) interpolate
                            : res[n / 2];                // otherwise take middle value
  kpi.stddev = std::sqrt(sq_sum / (n - 1));
  kpi.max_val = max_val;
  kpi.min_val = min_val;
  kpi.p95 = percentile(res, 0.95); // NOLINT(readability-magic-*)

  return kpi;
}

/**
 * @brief Returns @c true if @p addr is the IPv4 loopback address.
 *
 * Used to detect same-machine measurements, which may warrant different
 * reporting or behaviour from cross-host runs.
 *
 * @param addr  Dotted-decimal IPv4 string to test.
 * @return @c true if @p addr equals @c "127.0.0.1", @c false otherwise.
 */
[[nodiscard]] inline bool is_localhost(const std::string_view addr) {
  return addr == "127.0.0.1";
}

constexpr size_t MIN_MESSAGE_SIZE = sizeof(Payload); ///< Minimum datagram size. At least sizeof(@c Payload)
constexpr size_t MAX_MESSAGE_SIZE = 4096;            ///< Maximum supported datagram size in bytes.
const std::array<char, 8> SPINNER = {'-', '\\', '|', '/',
                                     '-', '\\', '|', '/'}; ///< Glyph sequence for a console spinner animation.
