#ifndef UNICODE
#define UNICODE
#endif

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
#include <string>
#include <thread>

std::atomic<bool> SHOULD_RUN = false;

using namespace std::chrono_literals;

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

struct SockRAII {
  SOCKET sock;

  SockRAII() : sock(INVALID_SOCKET) {}
  ~SockRAII() {
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

BOOL WINAPI sig_handler(DWORD sig) {
  switch (sig) {
  case CTRL_C_EVENT:
    SHOULD_RUN.store(false, std::memory_order_release);
    return TRUE;
  default:
    return FALSE;
  }
}

[[nodiscard]] double update_ema(double old_ema, double new_value) {
  constexpr int SAMPLES = 20;
  const double WEIGHT = 1.0 / static_cast<double>(SAMPLES - 1);

  return old_ema * (1 - WEIGHT) + new_value * WEIGHT;
}

int main(int argc, char **argv) {
  const char *addr = "192.168.1.10"; // receiver addr
  const uint16_t port = 16388;        // receiver port

  // initialize winsock
  WSARAII wsa;
  if (!wsa.init()) {
    std::cerr << "could not initialize wsa\n";
    return 1;
  }

  // initialize the socket
  SockRAII sock;
  if (!sock.init()) {
    std::cerr << "could not initialize socket\n";
    return 1;
  }

  // initialize the address
  struct sockaddr_in receiver;
  ZeroMemory(&receiver, sizeof(receiver));
  receiver.sin_family = AF_INET;
  receiver.sin_port = htons(port);
  inet_pton(AF_INET, addr, &receiver.sin_addr);

  // setup done, start sending
  std::cout << std::format("Setup done sending on {}:{}\n", addr, port);
  SetConsoleCtrlHandler(sig_handler, TRUE); // handle ctrl+c
  SHOULD_RUN.store(true);

  // send data
  struct Msg {
    uint32_t num;
  };
  Msg msg{.num = 0};

  uint32_t sends = 0;
  uint32_t errors = 0;
  double ema = 0.0;

  std::chrono::steady_clock::time_point next_cycle_start =
      std::chrono::steady_clock::now();                 // next cycle start
  std::chrono::steady_clock::time_point last_send_time; // last actual send time
  constexpr auto cycle_time = 2ms;
  while (SHOULD_RUN.load(std::memory_order_acquire)) {
    while (next_cycle_start < std::chrono::steady_clock::now()) {
      next_cycle_start += cycle_time;
    }

    // spin wait to get high precision sleep
    while (std::chrono::steady_clock::now() < next_cycle_start) {
      std::this_thread::yield();
    }

    if (sendto(sock.sock,                            // socket
               reinterpret_cast<const char *>(&msg), // buf
               sizeof(msg),                          // buf len
               0,                                    // flags
               (struct sockaddr *)&receiver,         // receiver
               sizeof(receiver)) == SOCKET_ERROR) {  // receiver struct size
      errors++;
      continue;
    }
    // will be nonsense on first send
    const double dt_send =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      last_send_time)
            .count();
    last_send_time = std::chrono::steady_clock::now();

    // calculate ema for sends
    if (sends == 0) {
      ema = dt_send;
    } else {
      ema = update_ema(ema, dt_send);
    }

    msg.num++;
    sends++;

    // print status sometimes
    if ((sends + errors) % 1000 == 0) {
      std::cout << std::format("Sent: {} Errors: {} EMA [Hz]: {:.2f}\n", sends,
                               errors, 1.0 / ema);
    }
  }

  std::cout << "Ending program!\n";

  return 1;
}
