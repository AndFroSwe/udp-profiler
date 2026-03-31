#ifndef UNICODE
#define UNICODE
#endif

#include "CLI/CLI.hpp"
#include "common.h"
#include "connection.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <string>

using namespace std::chrono_literals;
using namespace std::chrono;

// handle ctrl+c signals
#include <consoleapi.h>
std::atomic<bool> g_should_run = true; // send loop run flag
BOOL WINAPI sig_handler(DWORD sig) {
  switch (sig) {
  case CTRL_C_EVENT:
    g_should_run.store(false, std::memory_order_release);
    return TRUE;
  default:
    return FALSE;
  }
}

// parameters
constexpr int VERSION_MAJOR = 1;       // major version
constexpr int VERSION_MINOR = 0;       // minor version
constexpr auto PRINT_INTERVAL = 1s;    // how often to print status
constexpr int SOCKET_TIMEOUT_MS = 500; // how long to wait for socket to do something

// main program
int main(int argc, char **argv) {
  // setup app options
  CLI::App app{R"(RTT Client - Receives UDP packets from the server and echoes them back.


Listens on a given address and port for incoming packets from rtt-server,
timestamps them on arrival, and sends them back to the server.

Example:
  rtt-client -a 192.168.1.10 -p 5000)"};

  argv = app.ensure_utf8(argv);
  app.set_version_flag("-v,--version", std::format("{}.{}", VERSION_MAJOR, VERSION_MINOR), "Print version and exit");

  std::string addr;
  app.add_option("-a,--address", addr, "IP address to send to")->default_val("127.0.0.1");

  uint16_t port = 0;
  app.add_option("-p,--port", port, "Receiver port")
      ->required(true)
      ->check(CLI::Range(static_cast<uint16_t>(0), UINT16_MAX));

  uint32_t timeout_ms = 0;
  app.add_option(
         "-t,--timeout", timeout_ms,
         std::format("How many ms to wait for receive before breaking measurement (rounded up to nearest {} ms)",
                     SOCKET_TIMEOUT_MS))
      ->default_val(2000); // NOLINT(readability-magic-*)

  CLI11_PARSE(app, argc, argv);

  // start initializing
  // initialize winsock
  std::cout << std::format("Starting rtt-client v{}.{}\n", VERSION_MAJOR, VERSION_MINOR);

  // initialize the socket
  Connection sock;
  if (!sock.init(SOCKET_TIMEOUT_MS)) {
    std::cerr << "could not initialize socket\n";
    return 1;
  }

  // setup the local listen port
  if (!sock.bind_local(addr, port)) {
    std::cerr << "could not bind to local port";
    return 1;
  }

  // setup done, start sending
  std::cout << std::format("Setup done, listening on {}:{}. CTRL+C to quit.\n", addr, port);
  SetConsoleCtrlHandler(sig_handler, TRUE); // handle ctrl+c
  g_should_run.store(true);                 // activate write cycle

  // allocate data buffer
  auto buf = std::vector<std::byte>(MAX_MESSAGE_SIZE); // reserve size to fit a message
  memset(buf.data(), 0, buf.size());                   // zero out all data

  // send loop
  int64_t bounces = 0;                               // number of bounces in a measurement
  size_t i = 0;                                      // total number of cycles
  const auto wait_for = milliseconds(timeout_ms);    // wait for this long before considering measurement done
  auto last_measurement = steady_clock::now();       // last time successful measurement was reached
  auto start_measurement_time = steady_clock::now(); // keep track of measurment time
  auto next_print = steady_clock::now();             // keep track of status prints
  while (g_should_run.load(std::memory_order_acquire)) {
    i++;

    // wait incoming message
    const auto res = sock.receive_on_local_and_save_remote(buf);
    const auto receive_time = get_steady_timestamp_ns(); // save receive timestamp directly

    switch (res.ret) {
    case ReturnCode::TIMEOUT:
      if (bounces == 0) {
        // no connection yet, wait for connection
        std::cout << std::format("\rWaiting for connection [{}]", SPINNER[i % SPINNER.size()]);
      } else {
        if (steady_clock::now() - last_measurement > wait_for) {
          // timeout reached while measuring, measurement done
          const auto measured_for = last_measurement - start_measurement_time;
          std::cout << std::format("\nMeasurement done! Bounced {} times for {:.1f} s, restarting...\n", bounces,
                                   duration<double>(measured_for).count());
          // reset status
          bounces = 0;         // reset
          sock.reset_remote(); // prepare for new connection
        }
      }
      continue; // restart loop
    case ReturnCode::OK:
      break;
    case ReturnCode::ICMP:
    case ReturnCode::WSA_ERROR:
    case ReturnCode::SOCKET_INIT_ERROR:
    case ReturnCode::SEND_ERROR:
    case ReturnCode::RECEIVE_ERROR:
    case ReturnCode::NO_REMOTE:
    case ReturnCode::NO_LOCAL:
    case ReturnCode::WRONG_SIZE:
      continue; // ignore other errors, restart
    }

    // got message, start measuring time
    if (bounces == 0) {
      start_measurement_time = steady_clock::now();
    }

    // set new data to buffer
    // todo: receive size check should be at least payload
    Payload *payload = reinterpret_cast<Payload *>(buf.data());
    payload->client_receive_timestamp_ns = receive_time;
    payload->client_send_timestamp_ns = get_steady_timestamp_ns();

    // send reply
    if (const auto res = sock.send_to_remote(buf, payload->message_size); res.ret != ReturnCode::OK) {
      continue; // error went bad, restart
    }
    last_measurement = steady_clock::now(); // save time
    bounces++;                              // successful bounce

    // save the print for here to not disturb the first measurement
    if (steady_clock::now() > next_print) {
      while (next_print < steady_clock::now()) {
        next_print += PRINT_INTERVAL;
      }

      const auto remote_info = sock.get_remote_info();
      if (remote_info) {
        std::cout << std::format("\r    Bouncing {} bytes to {}:{}. Bounces: {}", //
                                 payload->message_size,                           // payload size
                                 remote_info->ip, remote_info->port,              // remote info
                                 bounces);                                        // bounces
      } else {
        assert(false && "should have remote here");
      }
    }
  }

  std::cout << "\nEnding program!\n";

  return 0;
}
