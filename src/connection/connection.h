/**
 * @file connection.h
 * @brief UDP socket connection abstraction
 *
 * Provides a platform-agnostic interface for UDP send/receive operations.
 * The implementation detail (e.g. Winsock2 on Windows) is hidden behind a private
 * @c Impl struct
 *
 * Typical usage:
 * @code
 * Connection conn;
 * conn.init(1000);                          // 1 s receive timeout
 * conn.bind_local("0.0.0.0", 9000);        // listen on all interfaces
 * conn.create_remote("192.168.1.10", 9001);
 *
 * std::vector<std::byte> buf(1024);
 * auto result = conn.receive_on_local(buf);
 * if (result.ret == ReturnCode::OK) { ... }
 * @endcode
 */

#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief Status codes returned by socket operations.
 *
 * Every @c TranscieveResult carries one of these values so callers can
 * distinguish between the various failure modes without inspecting
 * platform-specific error codes.
 */
enum class ReturnCode : uint8_t {
  OK,                ///< Operation completed successfully.
  WSA_ERROR,         ///< Windows Sockets (WSA) initialisation failed.
  SOCKET_INIT_ERROR, ///< Socket creation or configuration failed.
  SEND_ERROR,        ///< General failure during @c sendto.
  RECEIVE_ERROR,     ///< General failure during @c recv / @c recvfrom.
  NO_REMOTE,         ///< A remote endpoint must be set before sending.
  NO_LOCAL,          ///< A local endpoint must be bound before receiving.
  TIMEOUT,           ///< Receive timed out (no data within the configured window).
  ICMP,              ///< ICMP "port unreachable" — no listener at the remote address.
  WRONG_SIZE,        ///< The datagram size did not match the supplied buffer.
  INTERRUPTED,       ///< Socket was interrupted
};

/**
 * @brief Result type for send and receive operations.
 *
 * Both the byte count and the status code are always populated so callers
 * can inspect either field independently.
 */
struct TranscieveResult {
  int bytes;      ///< Number of bytes transferred or OS return code
  ReturnCode ret; ///< Return code of the operation
};

/**
 * @brief UDP socket wrapper with separate local/remote endpoint management.
 *
 * @c Connection owns one UDP socket and optional local / remote
 * @c sockaddr_in endpoints.  The socket is created in @c init() and
 * destroyed in @c close() or the destructor.
 *
 * The class is non-copyable (unique_ptr member) but can be moved if needed.
 * All public methods are safe to call after a failed @c init(); they return
 * sensible error codes rather than crashing.
 *
 * @note Only IPv4 UDP is supported.
 */
class Connection {
public:
  /**
   * @brief Holds a resolved IPv4 address/port pair.
   */
  struct ip_addr {
    std::string ip; ///< Dotted-decimal IPv4 string (e.g. @c "192.168.1.1").
    uint16_t port;  ///< Port number in host byte order.
  };

  /**
   * @brief Constructs a @c Connection with no socket or endpoints configured.
   *
   * Call @c init() before any other method.
   */
  Connection() noexcept;

  // Copy, not allowed
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;

  // Move
  Connection(Connection &&) noexcept;
  Connection &operator=(Connection &&) noexcept;

  /**
   * @brief Destroys the connection, closing the socket if still open.
   */
  ~Connection() noexcept;

  /**
   * @brief Closes the underlying socket and marks it as invalid.
   *
   * Safe to call multiple times. After @c close() the object can be
   * re-initialised with another call to @c init().
   */
  void close();

  /**
   * @brief Initialises Winsock and creates the UDP socket.
   *
   * Must be called once before @c bind_local(), @c create_remote(), or any
   * send/receive operation.  Calling @c init() a second time on the same
   * object (without an intervening @c close()) returns @c false.
   *
   * @param socket_timeout_ms  Receive timeout in milliseconds applied via
   *                           @c SO_RCVTIMEO.  A value of @c 0 disables the
   *                           timeout (blocking receive).
   * @return @c true on success; @c false if WSA startup or socket creation
   *         fails.
   */
  bool init(int socket_timeout_ms);

  /**
   * @brief Binds the socket to a local IPv4 address and port.
   *
   * Required before any receive operation.  May only be called after a
   * successful @c init().
   *
   * @param ip    Dotted-decimal IPv4 address string (e.g. @c "0.0.0.0" to
   *              listen on all interfaces).
   * @param port  Port number in host byte order.
   * @return @c true if the address was parsed and @c bind() succeeded;
   *         @c false otherwise.
   */
  bool bind_local(std::string_view ip, uint16_t port);

  /**
   * @brief Configures the remote endpoint for subsequent @c send_to_remote()
   *        calls.
   *
   * Does not open a connection — UDP is connectionless.  The stored address
   * is simply used as the destination for @c sendto().
   *
   * @param ip    Dotted-decimal IPv4 address string of the remote host.
   * @param port  Remote port number in host byte order.
   * @return @c true if the address string was parsed successfully;
   *         @c false on parse error.
   */
  bool create_remote(std::string_view ip, uint16_t port);

  /**
   * @brief Sends a datagram to the configured remote endpoint.
   *
   * @c create_remote() must have been called (or a remote must have been
   * captured via @c receive_on_local_and_save_remote()) before calling this.
   *
   * @param buf      Buffer whose contents are sent.
   * @param bufsize  (Optional) Number of bytes to send.  When empty the entire buffer
   *                 (@c buf.size()) is sent.
   * @return A @c TranscieveResult with @c ReturnCode::OK and the byte count
   *         on success, or an appropriate error code otherwise.
   */
  TranscieveResult send(const std::vector<std::byte> &buf, std::optional<size_t> bufsize = {});

  /**
   * @brief Receives a datagram on the bound local socket.
   *
   * Blocks until a datagram arrives, the configured timeout elapses, or an
   * error occurs.  The remote endpoint must already be known (set via
   * @c create_remote() or a prior @c receive_on_local_and_save_remote() call);
   * use @c receive_on_local_and_save_remote() to accept from an unknown sender.
   *
   * @param[out] buf  Buffer to write received data into.  Must be pre-sized
   *                  to the maximum expected datagram length.
   * @return A @c TranscieveResult with @c ReturnCode::OK and the byte count
   *         on success, or an error code (@c TIMEOUT, @c ICMP, @c WRONG_SIZE,
   *         etc.) on failure.
   */
  TranscieveResult receive(std::vector<std::byte> &buf) const;

  /**
   * @brief Receives a datagram and captures the sender as the new remote.
   *
   * If no remote endpoint is currently configured, uses @c recvfrom() to
   * accept a datagram from any sender and stores that sender's address for
   * future send/receive operations.  If a remote is already configured,
   * behaves identically to @c receive_on_local().
   *
   * @param[out] buf  Buffer to write received data into.  Must be pre-sized
   *                  to the maximum expected datagram length.
   * @return A @c TranscieveResult with @c ReturnCode::OK and the byte count
   *         on success, or an error code on failure.
   */
  TranscieveResult receive_on_local_and_save_remote(std::vector<std::byte> &buf);

  /**
   * @brief Clears the stored remote endpoint.
   *
   * After this call, @c send_to_remote() will fail with @c ReturnCode::NO_REMOTE
   * and the next @c receive_on_local_and_save_remote() will use @c recvfrom()
   * again to capture a new sender.
   */
  void reset_remote();

  /**
   * @brief Returns the currently configured remote endpoint, if any.
   *
   * @return An @c ip_addr with the remote's dotted-decimal IPv4 string and
   *         port, or @c std::nullopt if no remote has been set.
   */
  std::optional<ip_addr> get_remote_info();

private:
  struct Impl;                  ///< OS-specific socket state (Winsock2).
  std::unique_ptr<Impl> m_impl; ///< PIMPL handle; never null after construction.
};
