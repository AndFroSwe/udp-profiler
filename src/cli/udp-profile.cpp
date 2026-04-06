#ifndef UNICODE
#define UNICODE
#endif

#include "CLI/CLI.hpp"
#include "common.h"
#include "connection.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <format>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// globals
volatile sig_atomic_t g_should_run = 1; // main loop flag

// parameters
constexpr int VERSION_MAJOR = 1;        // major version
constexpr int VERSION_MINOR = 1;        // minor version
constexpr int VERSION_BUG = 0;          // bugfix version
constexpr int SOCKET_TIMEOUT_MS = 1000; // timeout for socket receive

// main program
int main(int argc, char **argv) {
  // setup app options
  CLI::App app{R"(UDP Profiling sender program

  Sends UDP packets to a target address at a specified frequency.
  Used for profiling network latency and packet loss.

  Sends a byte array with the 4 LSBs as a uint32_t counter 
  that is strictly increasing with each message. The rest 
  of the size is padded out with 0.

  Example:
    udp_sender -a 192.168.1.10 -p 5000 -f 1000 -u 2.0 -s 128
  )"};
  argv = app.ensure_utf8(argv);
  app.set_version_flag("-v,--version", std::format("{}.{}.{}", VERSION_MAJOR, VERSION_MINOR, VERSION_BUG),
                       "Print version and exit");

  std::string addr;
  app.add_option("-a,--address", addr, "IP address to send to")->default_val("127.0.0.1");

  uint16_t port = 0;
  app.add_option("-p,--port", port, "Receiver port")
      ->required(true)
      ->check(CLI::Range(static_cast<uint16_t>(0), static_cast<uint16_t>(UINT16_MAX)));

  uint32_t freq = 0;
  app.add_option("-f,--freq", freq, "Send frequency [Hz]")->default_val(500); // NOLINT(readability-magic*)

  uint32_t message_size = 0;
  app.add_option("-s,--size", message_size, "Message size in bytes")
      ->default_val(64)                            // NOLINT(readability-magic*)
      ->check(CLI::Range(static_cast<uint32_t>(4), // needs to fit at least counter
                         UINT32_MAX));

  double print_update_time = 0.0;
  app.add_option("-u,--update", print_update_time, "Time between update printouts [s]")->default_val(1.0);

  CLI11_PARSE(app, argc, argv);

  // initialize the socket
  Connection sock;
  if (!sock.init(SOCKET_TIMEOUT_MS)) {
    std::cerr << "could not initialize socket\n";
    return 1;
  }

  // initialize the address
  if (!sock.create_remote(addr, port)) {
    std::cerr << "could not set remote\n";
    return 1;
  }

  // register ctrl c handler
  g_should_run = 1;
  std::signal(SIGINT, [](int sig) {
    if (sig == SIGINT) {
      g_should_run = 0;
    }
  });

  // setup done, start sending
  std::cout << std::format("Setup done!\nSending {} byte to {}:{} @ {} Hz\n", message_size, addr, port, freq);

  // send data definition
  // allocate padded buffer
  auto buf = std::vector<std::byte>(message_size);
  memset(buf.data(), 0, buf.size()); // zero out all data

  uint32_t sends = 0;  // number of successful sends
  uint32_t errors = 0; // number of unseccussful sends
  double ema = 0.0;    // Exponential moving average of send Hz

  auto next_cycle_start = std::chrono::steady_clock::now(); // next cycle start
  std::chrono::steady_clock::time_point last_send_time;     // last actual send time
  const auto cycle_time = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::duration<double>(1.0 / static_cast<double>(freq)));
  const auto print_wait_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(print_update_time));
  auto next_printout_time = std::chrono::steady_clock::now();

  // send loop
  while (g_should_run == 1) {
    // calculate next send
    while (next_cycle_start < std::chrono::steady_clock::now()) {
      next_cycle_start += cycle_time;
    }

    // spin wait to get high precision sleep
    while (std::chrono::steady_clock::now() < next_cycle_start) {
      std::this_thread::yield();
    }

    // actual send
    memcpy(buf.data(), &sends, sizeof(sends));
    if (const auto res = sock.send(buf); res.ret != ReturnCode::OK) {
      errors++;
      continue;
    }

    // save time since last send. will be nonsense on first send
    const double dt_send = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_send_time).count();
    last_send_time = std::chrono::steady_clock::now();

    // calculate ema for sends
    if (sends == 0) {
      ema = dt_send;
    } else {
      ema = update_ema(ema, dt_send);
    }

    sends++;

    // print status sometimes
    if (std::chrono::steady_clock::now() >= next_printout_time) {
      std::cout << std::format("\rSent: {} Errors: {} EMA [Hz]: {:4.2f}", sends, errors, 1.0 / ema);
      // calculate next printout time
      while (next_printout_time < std::chrono::steady_clock::now()) {
        next_printout_time += print_wait_time;
      }
    }
  }

  std::cout << "\nEnding program!\n";

  return 0;
}
