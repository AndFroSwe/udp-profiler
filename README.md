# UDP Profiler

A simple UDP round-trip time (RTT) profiling tool for measuring network latency between two endpoints.

## Overview

The tool consists of two programs:

- **rtt-server** — sends timestamped UDP packets and collects RTT statistics
- **rtt-client** — listens for incoming packets, timestamps them, and echoes them back

The server sends packets at a configurable frequency, waits for the client to echo them back, and computes RTT and client-to-server (CtS) timing statistics when the run is complete.

## Usage

Start the client first, then the server.

**Client:**
```
rtt-client -a <address> -p <port>
```

**Server:**
```
rtt-server -a <address> -p <port> [options]
```

Run either program with `--help` for the full list of options.

### Server options

| Flag | Description | Default |
|------|-------------|---------|
| `-a, --address` | IP address to send to | `127.0.0.1` |
| `-p, --port` | Receiver port | *(required)* |
| `-f, --freq` | Send frequency [Hz] | `500` |
| `-s, --size` | Message size [bytes] | `64` |
| `-c, --count` | Number of cycles to run | `100` |
| `-u, --update` | Time between status printouts [s] | `1.0` |

### Example
```
rtt-client -a 192.168.1.10 -p 5000
rtt-server -a 192.168.1.10 -p 5000 -f 500 -s 64 -c 1000
```

## Output

After a run the server prints RTT and client-to-server timing statistics:
```
Measurement results:
------------------------
Sends: 1000
Errors: 0
               RTT      CtS
Mean [us]    123.4    61.2
Median [us]  120.1    60.0
Stddev [us]    8.3     4.1
Max [us]     201.5   100.8
Min [us]     110.2    55.3
P95 [us]     145.6    72.3
```

Press `Ctrl+C` at any time to stop either program early.

## Compilation

Compile with CMake:
```
cmake -B build
cmake --build build
```

## Requirements

- Windows (uses Winsock)
- C++20 or later

## Author

Andreas Fröderberg
