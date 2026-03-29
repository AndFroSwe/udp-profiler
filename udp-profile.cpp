#ifndef UNICODE
#define UNICODE
#include <string>
#include <vector>
#endif

#include "CLI/CLI.hpp"

#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <Windows.h>
#include <timeapi.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <thread>

std::atomic<bool> SHOULD_RUN = true; // send loop run flag
constexpr int VERSION_MAJOR = 1;     // major version
constexpr int VERSION_MINOR = 0;     // minor version

using namespace std::chrono_literals;

// windows sockets wsa raii helper
struct WSARAII {
  WSAData wsa;
  bool is_init;

  WSARAII() : is_init(false) {}
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
    return true;
  }
};

// socket raii helper class
struct Connection {
  SOCKET sock;

  Connection() : sock(INVALID_SOCKET) {}
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
};

// handle ctrl+c signals
BOOL WINAPI sig_handler(DWORD sig) {
  switch (sig) {
  case CTRL_C_EVENT:
    SHOULD_RUN.store(false, std::memory_order_release);
    return TRUE;
  default:
    return FALSE;
  }
}

// ema calculation
[[nodiscard]] constexpr double update_ema(double old_ema, double new_value) {
  constexpr int SAMPLES = 20;
  constexpr double WEIGHT = 1.0 / static_cast<double>(SAMPLES - 1);

  return old_ema * (1 - WEIGHT) + new_value * WEIGHT;
}

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
      ->check(CLI::Range(static_cast<uint32_t>(4), // needs to fit at least counter
                         UINT32_MAX));

  double print_update_time = 0.0;
  app.add_option("-u,--update", print_update_time, "Time between update printouts [s]")->default_val(1.0);

  CLI11_PARSE(app, argc, argv);

  // start initializing
  // initialize winsock
  std::cout << std::format("Starting udp-profiler v{}.{}\n", VERSION_MAJOR, VERSION_MINOR);
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

  // initialize the address
  struct sockaddr_in receiver;
  ZeroMemory(&receiver, sizeof(receiver));
  receiver.sin_family = AF_INET;
  receiver.sin_port = htons(port);
  inet_pton(AF_INET, addr.c_str(), &receiver.sin_addr);

  // setup done, start sending
  std::cout << std::format("Setup done!\nSending {} byte to {}:{} @ {} Hz\n", message_size, addr, port, freq);
  SetConsoleCtrlHandler(sig_handler, TRUE); // handle ctrl+c
  SHOULD_RUN.store(true);

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
  while (SHOULD_RUN.load(std::memory_order_acquire)) {
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
    if (sendto(sock.sock,                                  // socket
               reinterpret_cast<const char *>(buf.data()), // buf
               static_cast<int>(buf.size()),               // buf len
               0,                                          // flags
               (struct sockaddr *)&receiver,               // receiver
               sizeof(receiver)) == SOCKET_ERROR) {        // receiver struct size
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
