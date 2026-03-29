#ifndef UNICODE
#define UNICODE
#endif

#include "CLI/CLI.hpp"
#include "common.h"

#include <timeapi.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef NDEBUG
#define DBG(s)
#else
#define DBG(s) std::cout << s << '\n';
#endif

using namespace std::chrono_literals;

// handle ctrl+c signals
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
constexpr int VERSION_MAJOR = 1;        // major version
constexpr int VERSION_MINOR = 0;        // minor version
constexpr int SOCKET_TIMEOUT_MS = 1000; // socket receive timeout

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

  CLI11_PARSE(app, argc, argv);

  // start initializing
  // initialize winsock
  std::cout << std::format("Starting rtt-client v{}.{}\n", VERSION_MAJOR, VERSION_MINOR);
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

  // setup the local listen port
  if (!sock.bind_local(addr, port)) {
    std::cerr << "could not bind to local port";
    return 1;
  }

  // set socket timeout
  DWORD timeout_ms = SOCKET_TIMEOUT_MS; // 1 second
  if (setsockopt(sock.sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms)) ==
      SOCKET_ERROR) {
    std::cerr << "could not set socket timeout\n";
    return 1;
  }

  // setup done, start sending
  std::cout << std::format("Setup done, listening on {}:{}\n", addr, port);
  SetConsoleCtrlHandler(sig_handler, TRUE); // handle ctrl+c
  g_should_run.store(true);                 // activate write cycle

  // allocate data buffer
  auto buf = std::vector<std::byte>(MAX_MESSAGE_SIZE); // reserve size to fit a message
  memset(buf.data(), 0, buf.size());                   // zero out all data

  // send loop
  int64_t bounces = 0;
  while (g_should_run.load(std::memory_order_acquire)) {
    // wait incoming message

    int bytes = 0;
    // on first receive, save the sender
    if (!sock.remote.has_value()) {
      int client_len = sizeof(sock.remote.value());
      sockaddr_in remote;
      ZeroMemory(&remote, sizeof(remote));
      bytes = recvfrom(sock.sock,                             // socket
                       reinterpret_cast<char *>(buf.data()),  // recv buf. reuse send buf
                       static_cast<int>(buf.size()),          // buffer size
                       0,                                     // flags
                       reinterpret_cast<sockaddr *>(&remote), // client addr
                       &client_len);                          // length of client addr
      if (bytes != SOCKET_ERROR) {
        sock.remote = remote; // save the remote
      }
    } else {
      // just receive, we know the remote
      bytes = recv(sock.sock,                            // socket
                   reinterpret_cast<char *>(buf.data()), // recv buf. reuse send buf
                   static_cast<int>(buf.size()),         // buffer size
                   0);                                   // flags
    }
    const auto receive_time = get_timestamp_ns(); // save receive timestamp directly

    // handle errors
    if (bytes == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAETIMEDOUT) {
        // handle timeout
        if (bounces == 0) {
          std::cout << "Got receive timeout, trying again...\n";
        } else {
          std::cout << "Measurement done, exiting...\n";
          break;
        }
      } else {
        // handle other errors
      }
      continue;
    }

    // set new data to buffer
    // todo: receive size check should be at least payload
    Payload *payload = reinterpret_cast<Payload *>(buf.data());
    payload->client_receive_timestamp_ns = receive_time;

    // send reply
    if (sock.send_to_remote(reinterpret_cast<const char *>(buf.data()), static_cast<int>(payload->message_size)) ==
        SOCKET_ERROR) {
      continue;
    }

    bounces++;
  }

  // print measurements
  std::cout << std::format("Bounced {} times\n", bounces);

  std::cout << "\nEnding program!\n";

  return 0;
}
