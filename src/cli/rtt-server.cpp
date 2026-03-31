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
#include <thread>
#include <vector>

using namespace std::chrono_literals;

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
constexpr int SOCKET_TIMEOUT_MS = 500; // socket receive timeout

// main program
int main(int argc, char **argv) {
  // setup app options
  CLI::App app{R"(RTT Server - Sends UDP packets to a client and measures round-trip time (RTT).

Sends timestamped UDP packets to an rtt-client at a given address and port,
waits for the client to echo them back, and computes RTT and client-to-server
timing statistics.

Example:
  rtt-server -a 192.168.1.10 -p 5000 -f 500 -s 64 -c 1000)"};

  argv = app.ensure_utf8(argv);
  app.set_version_flag("-v,--version", std::format("{}.{}", VERSION_MAJOR, VERSION_MINOR), "Print version and exit");

  std::string addr;
  app.add_option("-a,--address", addr, "IP address to send to")->default_val("127.0.0.1");

  uint16_t port = 0;
  app.add_option("-p,--port", port, "Receiver port")
      ->required(true)
      ->check(CLI::Range(static_cast<uint16_t>(0), UINT16_MAX));

  uint32_t freq = 0;
  app.add_option("-f,--freq", freq, "Send frequency [Hz]")->default_val(500); // NOLINT(readability-magic*)

  uint32_t message_size = 0;
  app.add_option("-s,--size", message_size, "Message size in bytes")
      ->default_val(64)                                             // NOLINT(readability-magic*)
      ->check(CLI::Range(static_cast<uint32_t>(MIN_MESSAGE_SIZE),   // needs to fit at least payload
                         static_cast<uint32_t>(MAX_MESSAGE_SIZE))); // and not be bigger that max allowed size

  uint32_t cycles = 0;
  app.add_option("-c,--count", cycles, "Cycles to run test for")->default_val(500); // NOLINT(readability-magic*)

  double print_update_time = 0.0;
  app.add_option("-u,--update", print_update_time, "Time between update printouts [s]")->default_val(1.0);

  CLI11_PARSE(app, argc, argv);

  // start initializing
  // initialize winsock
  std::cout << std::format("Starting rtt-server v{}.{}\n", VERSION_MAJOR, VERSION_MINOR);
  WSARAII wsa;
  if (!wsa.init()) {
    std::cerr << "could not initialize wsa\n";
    return 1;
  }

  // initialize the socket
  Connection sock;
  if (!sock.init(SOCKET_TIMEOUT_MS)) {
    std::cerr << "could not initialize socket\n";
    return 1;
  }

  // setup the remote target
  if (!sock.create_remote(addr, port)) {
    std::cerr << "incorrect receiver address settings\n";
    return 1;
  }

  // bind to local port to listen
  if (!sock.bind_local(addr, port + 1)) {
    std::cerr << "could not bind to local address";
    return 1;
  }

  // setup done, start sending
  std::cout << std::format("Setup done!\nSending {} byte to {}:{} @ {} Hz {} times\n", message_size, addr, port, freq,
                           cycles);
  SetConsoleCtrlHandler(sig_handler, TRUE); // handle ctrl+c
  g_should_run.store(true);                 // activate write cycle

  // allocate data buffer
  auto buf = std::vector<std::byte>(message_size);
  memset(buf.data(), 0, buf.size()); // zero out all data

  // prepare
  // cycle time
  const auto cycle_time = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::duration<double>(1.0 / static_cast<double>(freq))); // calculate cycle time
  auto next_cycle_start = std::chrono::steady_clock::now();            // init next cycle start
  double ema_send_freq = 0.0;                                          // keep track of send frequency
  auto last_send = std::chrono::steady_clock::now();                   // last send, prepare value

  // print time
  const auto print_wait_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(print_update_time));
  auto next_printout_time = std::chrono::steady_clock::now();

  // initialize measurement
  Measurement measure = {
      .sends = 0,
      .errors = 0,
  };
  measure.server_send_timestamp_ns.reserve(cycles);    // reserver space for measurements
  measure.client_receive_timestamp_ns.reserve(cycles); // reserve space for measurements
  measure.client_send_timestamp_ns.reserve(cycles);    // reserver space for measurements
  measure.server_receive_timestamp_ns.reserve(cycles); // reserve space for measurements

  // send loop
  uint32_t i = 0;                                        // loop increment
  uint32_t message_id = 0;                               // initialize
  while (g_should_run.load(std::memory_order_acquire) && // ctrl+c handler
         message_id < cycles)                            // desired attempts
  {
    i++;

    // calculate next send
    while (next_cycle_start < std::chrono::steady_clock::now()) {
      next_cycle_start += cycle_time;
    }

    // spin wait to get high precision sleep
    while (std::chrono::steady_clock::now() < next_cycle_start) {
      std::this_thread::yield();
    }

    // create the payload
    Payload payload{
        .message_id = message_id,         // add the message id
        .message_size = message_size,     // info about message size
        .server_send_timestamp_ns = 0,    // initialize
        .client_receive_timestamp_ns = 0, // initialize
        .client_send_timestamp_ns = 0,    // initialize
        .server_receive_timestamp_ns = 0, // initialize
    };
    payload.server_send_timestamp_ns = get_steady_timestamp_ns();
    memcpy(buf.data(), &payload, sizeof(payload)); // copy to beginning of buffer

    // send payload
    if (sock.send_to_remote(reinterpret_cast<const char *>(buf.data()), buf.size()) ==
        SOCKET_ERROR) { // receiver struct size
      measure.errors++;
      continue;
    }

    // wait for return message
    int bytes = sock.receive_on_local(reinterpret_cast<char *>(buf.data()), buf.size());
    const auto receive_time = get_steady_timestamp_ns(); // save receive timestamp on reception

    // handle errors
    if (bytes == SOCKET_ERROR) {
      const auto err = WSAGetLastError(); // get the error
      switch (err) {
      case WSAETIMEDOUT:
        if (measure.sends == 0) {
          // haven't gotten anything yet
          std::cout << std::format("\rWaiting for client [{}]", SPINNER[i % SPINNER.size()]);
        } else {
          // measurement in progress, log error on timeout
          measure.errors++;
        }
      case WSAECONNRESET:
        continue; // ICMP error on last send
      default:
        std::cerr << err << '\n' << std::flush;
        assert(false && "unknown WSA error code");
      }

      continue;
    }

    // decode received message
    if (bytes != message_size) {
      measure.errors++;
      continue;
    }
    memcpy(&payload, buf.data(), sizeof(payload));
    payload.server_receive_timestamp_ns = receive_time;

    // check that correct message was received
    if (payload.message_id != message_id) {
      measure.errors++;
      continue;
    }

    // print here to not disturb measurements
    if (measure.sends == 0) {
      std::cout << "\nFound client!\n";
    }

    // update measurements
    measure.server_send_timestamp_ns.emplace_back(payload.server_send_timestamp_ns);
    measure.client_receive_timestamp_ns.emplace_back(payload.client_receive_timestamp_ns);
    measure.client_send_timestamp_ns.emplace_back(payload.client_send_timestamp_ns);
    measure.server_receive_timestamp_ns.emplace_back(payload.server_receive_timestamp_ns);
    measure.sends++;

    // update measurement
    message_id++;

    // keep track of ema. needs at least 2 values to calculate
    const auto now = std::chrono::steady_clock::now();
    if (measure.sends > 1) {
      const auto send_freq = 1.0 / std::chrono::duration<double>(now - last_send).count();
      ema_send_freq = measure.sends == 2 ? send_freq                             // seed ema
                                         : update_ema(ema_send_freq, send_freq); // update recursively
    }
    last_send = now;

    // print status sometimes
    if (std::chrono::steady_clock::now() >= next_printout_time) {
      std::cout << std::format("\rMeasured {}/{} [{:2.0f} %] Current freq: {:6.2f} Hz", message_id, cycles,
                               static_cast<double>(message_id) / cycles * 100, // NOLINT(readability-magic*)
                               ema_send_freq);
      // calculate next printout time
      while (next_printout_time < std::chrono::steady_clock::now()) {
        next_printout_time += print_wait_time;
      }
    }
  } // end of work loop
  std::cout << '\n'; // get newline for final prints

  if (measure.sends == 0) {
    std::cout << "Got no values, exiting...";
    return 0;
  }

  std::cout << "Measurements done, calculating KPIs...\n";
  std::cout << "Round-Trip-Time RTT...\n";
  const auto kpi_rtt = calculate_kpis(elementwise(measure.server_receive_timestamp_ns, // T4
                                                  measure.server_send_timestamp_ns,    // T1
                                                  std::minus<int64_t>{}));             // take diff

  std::cout << "Client handling time (Tc)...\n";
  const auto kpi_tc = calculate_kpis(elementwise(measure.client_send_timestamp_ns,    // T3
                                                 measure.client_receive_timestamp_ns, // T2
                                                 std::minus<int64_t>{}));             // take diff

  // helper
  constexpr auto TO_US = [](const int64_t ns) -> double { // NOLINT(readability-*)
    return static_cast<double>(ns) / 1000.0;              // NOLINT(readability-magic*)
  };

  // print measurements
  std::cout << "\nMeasurement results:\n";
  std::cout << "------------------------\n";
  std::cout << std::format("Sends: {}\n", measure.sends);
  std::cout << std::format("Errors: {}\n", measure.errors);

  std::cout << "\n\t\t   RTT\t    Tc\n";
  std::cout << std::format("Mean [us]\t{:6.2f}\t{:6.2f}\n", TO_US(kpi_rtt.mean), TO_US(kpi_tc.mean));
  std::cout << std::format("Median [us]\t{:6.2f}\t{:6.2f}\n", TO_US(kpi_rtt.median), TO_US(kpi_tc.median));
  std::cout << std::format("Stddev [us]\t{:6.2f}\t{:6.2f}\n", TO_US(kpi_rtt.stddev), TO_US(kpi_tc.stddev));
  std::cout << std::format("Max [us]\t{:6.2f}\t{:6.2f}\n", TO_US(kpi_rtt.max_val), TO_US(kpi_tc.max_val));
  std::cout << std::format("Min [us]\t{:6.2f}\t{:6.2f}\n", TO_US(kpi_rtt.min_val), TO_US(kpi_tc.min_val));
  std::cout << std::format("P95 [us]\t{:6.2f}\t{:6.2f}\n", TO_US(kpi_rtt.p95), TO_US(kpi_tc.p95));
  std::cout << "\nEnding program!\n";

  return 0;
}
