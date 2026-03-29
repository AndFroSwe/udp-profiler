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

#ifdef NDEBUG
#define DBG(s)
#else
#define DBG(s) std::cout << s << '\n';
#endif

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
  std::cout << std::format("Setup done!\nSending {} byte to {}:{} @ {} Hz {} times\n", message_size, addr, port, freq,
                           cycles);
  SetConsoleCtrlHandler(sig_handler, TRUE); // handle ctrl+c
  SHOULD_RUN.store(true);                   // activate write cycle

  // allocate data buffer
  auto buf = std::vector<std::byte>(message_size);
  memset(buf.data(), 0, buf.size()); // zero out all data

  // prepare
  const auto cycle_time = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::duration<double>(1.0 / static_cast<double>(freq))); // calculate cycle time
  auto next_cycle_start = std::chrono::steady_clock::now();            // init next cycle start
  // initialize measurement
  Measurement measure = {
      .sends = 0,
      .errors = 0,
      .ema = 0,
      .min_rtt = 0,
      .max_rtt = 0,
  };

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
        .message_size = message_size,            // info about message size
        .send_timestamp_ns = get_timestamp_ns(), // set current timestamp
        .receive_timestamp_ns = 0,               // initialize
        .send_timestamp2_ns = 0,
        .receive_timestamp2_ns = 0,
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
      DBG("Send error");
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
        DBG("Receive error");
        // handle other errors
        measure.errors++;
      }
      continue;
    }

    // decode received message
    Payload final_payload;
    if (bytes != message_size) {
      DBG("Message size error");
      measure.errors++;
      continue;
    }
    memcpy(&final_payload, buf.data(), sizeof(final_payload));
    final_payload.receive_timestamp2_ns = receive_time;

    // check that correct message was received
    if (final_payload.message_id != message_id) {
      DBG("ID error");
      measure.errors++;
      continue;
    }

    // update measurements
    int64_t rtt = final_payload.receive_timestamp2_ns - final_payload.send_timestamp_ns;
    measure.sends++;
    measure.ema = measure.sends == 1 ? rtt : update_ema(measure.ema, rtt); // init ema to value on first run
    measure.min_rtt = measure.min_rtt == 0 ? rtt : std::min(measure.min_rtt, rtt);
    measure.max_rtt = std::max(measure.max_rtt, rtt);

    // update measurement
    message_id++;
  }

  // print measurements
  std::cout << "Measurement results:\n";
  std::cout << "------------------------\n";
  std::cout << std::format("Sends: {}\n", measure.sends);
  std::cout << std::format("Errors: {}\n", measure.errors);
  std::cout << std::format("RTT EMA: {} us\n", static_cast<double>(measure.ema) / 1000.0);
  std::cout << std::format("Max RTT: {} us\n", static_cast<double>(measure.max_rtt) / 1000.0);
  std::cout << std::format("Min RTT: {} us\n", static_cast<double>(measure.min_rtt) / 1000.0);

  std::cout << "\nEnding program!\n";

  return 0;
}
