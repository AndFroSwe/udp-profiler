#ifndef UNICODE
#define UNICODE
#endif

#include "CLI/CLI.hpp"
#include "common.h"

#include <timeapi.h>

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
std::atomic<bool> SHOULD_RUN = true; // send loop run flag
BOOL WINAPI sig_handler(DWORD sig) {
  switch (sig) {
  case CTRL_C_EVENT:
    SHOULD_RUN.store(false, std::memory_order_release);
    return TRUE;
  default:
    return FALSE;
  }
}

// parameters
constexpr int VERSION_MAJOR = 1;        // major version
constexpr int VERSION_MINOR = 0;        // minor version
constexpr int SOCKET_TIMEOUT_MS = 1000; // socket receive timeout

// main program
int main(int argc, char **argv) {
  // setup app options
  CLI::App app{R"(RTT Server)"};
  argv = app.ensure_utf8(argv);
  app.set_version_flag("-v,--version", std::format("{}.{}", VERSION_MAJOR, VERSION_MINOR), "Print version and exit");

  std::string addr = "";
  app.add_option("-a,--address", addr, "IP address to send to")->default_val("127.0.0.1");

  uint16_t port = 0;
  app.add_option("-p,--port", port, "Receiver port")
      ->required(true)
      ->check(CLI::Range(static_cast<uint16_t>(0), UINT16_MAX));

  uint32_t freq = 0;
  app.add_option("-f,--freq", freq, "Send frequency [Hz]")->default_val(500);

  uint32_t message_size = 0;
  app.add_option("-s,--size", message_size, "Message size in bytes")
      ->default_val(64)
      ->check(CLI::Range(static_cast<uint32_t>(MIN_MESSAGE_SIZE),   // needs to fit at least payload
                         static_cast<uint32_t>(MAX_MESSAGE_SIZE))); // and not be bigger that max allowed size

  uint32_t cycles = 0;
  app.add_option("-c,--count", cycles, "Cycles to run test for")->default_val(100);

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
  if (!sock.init()) {
    std::cerr << "could not initialize socket\n";
    return 1;
  }

  // set socket timeout
  DWORD timeout_ms = SOCKET_TIMEOUT_MS; // 1 second
  if (setsockopt(sock.sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms)) ==
      SOCKET_ERROR) {
    std::cerr << "could not set socket timeout\n";
    return 1;
  }

  // setup the remote target
  if (!sock.set_remote(addr, port)) {
    std::cerr << "incorrect receiver address settings\n";
    return 1;
  }

  // bind to local port to listen
  if (!sock.bind_local(addr, port + 1)) {
    std::cerr << "could not bind to local address";
    return 1;
  }

  // setup done, start sending
  std::cout << std::format("Setup done!\nSending {} byte to {}:{} @ {} Hz {} times\n\n", message_size, addr, port, freq,
                           cycles);
  SetConsoleCtrlHandler(sig_handler, TRUE); // handle ctrl+c
  SHOULD_RUN.store(true);                   // activate write cycle

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
  measure.rtt.reserve(cycles);              // reserve space for measurements
  measure.client_to_server.reserve(cycles); // reserver space for measurements

  // send loop
  int64_t message_id = 0;                              // initialize
  while (SHOULD_RUN.load(std::memory_order_acquire) && // ctrl+c handler
         message_id < cycles &&                        // desired attempts
         measure.errors + measure.sends < cycles)      // early escape
  {
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
        .message_size = message_size,                   // info about message size
        .server_send_timestamp_ns = get_timestamp_ns(), // set current timestamp
        .client_receive_timestamp_ns = 0,               // initialize
        .server_receive_timestamp_ns = 0,
        .message_id = message_id // add the message id
    };
    memcpy(buf.data(), &payload, sizeof(payload)); // copy to beginning of buffer

    // send payload
    if (sendto(sock.sock,                                          // socket
               reinterpret_cast<const char *>(buf.data()),         // buf
               static_cast<int>(buf.size()),                       // buf len
               0,                                                  // flags
               reinterpret_cast<sockaddr *>(&sock.remote.value()), // receiver
               sizeof(sock.remote.value())) == SOCKET_ERROR) {     // receiver struct size
      measure.errors++;
      continue;
    }

    // wait for return message
    int bytes = recv(sock.sock,                            // socket
                     reinterpret_cast<char *>(buf.data()), // recv buf. reuse send buf
                     static_cast<int>(buf.size()),         // buffer size
                     0);                                   // flags
    const auto receive_time = get_timestamp_ns();          // save receive timestamp on reception

    // handle errors
    if (bytes == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAETIMEDOUT) {
        // handle timeout
        if (measure.sends == 0) {
          std::cout << "Got receive timeout, trying again...\n";
        } else {
          measure.errors++;
        }
      } else {
        // handle other errors
        measure.errors++;
      }
      continue;
    }

    // decode received message
    Payload final_payload;
    if (bytes != message_size) {
      measure.errors++;
      continue;
    }
    memcpy(&final_payload, buf.data(), sizeof(final_payload));
    final_payload.server_receive_timestamp_ns = receive_time;

    // check that correct message was received
    if (final_payload.message_id != message_id) {
      measure.errors++;
      continue;
    }

    // update measurements
    measure.rtt.emplace_back(final_payload.server_receive_timestamp_ns - final_payload.server_send_timestamp_ns);
    measure.client_to_server.emplace_back(final_payload.server_receive_timestamp_ns -
                                          final_payload.client_receive_timestamp_ns);
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
                               static_cast<double>(message_id) / cycles * 100, ema_send_freq);
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
  std::cout << "RTT...\n";
  const auto rtt_kpi = calculate_kpis(measure.rtt);

  std::cout << "Client to Server times...\n";
  const auto rtt_cts = calculate_kpis(measure.client_to_server);
  std::cout << "Done!\n";

  // helper
  constexpr auto to_us = [](const int64_t ns) -> double { return static_cast<double>(ns) / 1000.0; };

  // print measurements
  std::cout << "Measurement results:\n";
  std::cout << "------------------------\n";
  std::cout << std::format("Sends: {}\n", measure.sends);
  std::cout << std::format("Errors: {}\n", measure.errors);

  std::cout << "\t\t   RTT\t   CtS\n";
  std::cout << std::format("Mean [us]\t{:6.1f}\t{:6.1f}\n", to_us(rtt_kpi.mean), to_us(rtt_cts.mean));
  std::cout << std::format("Median [us]\t{:6.1f}\t{:6.1f}\n", to_us(rtt_kpi.median), to_us(rtt_cts.median));
  std::cout << std::format("Stddev [us]\t{:6.1f}\t{:6.1f}\n", to_us(rtt_kpi.stddev), to_us(rtt_cts.stddev));
  std::cout << std::format("Max [us]\t{:6.1f}\t{:6.1f}\n", to_us(rtt_kpi.max_val), to_us(rtt_cts.max_val));
  std::cout << std::format("Min [us]\t{:6.1f}\t{:6.1f}\n", to_us(rtt_kpi.min_val), to_us(rtt_cts.min_val));
  std::cout << std::format("P95 [us]\t{:6.1f}\t{:6.1f}\n", to_us(rtt_kpi.p95), to_us(rtt_cts.p95));

  std::cout << "\nEnding program!\n";

  return 0;
}
