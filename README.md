# redis_cpp

> A Redis-compatible key-value server written in **C++20**, rebuilt from scratch with a focus on systems programming concepts and engineering best practices.

## ✨ Features

| Feature | Implementation |
|---------|---------------|
| **I/O Model** | Linux `epoll` & macOS `kqueue` edge-triggered + Reactor pattern |
| **Concurrency** | Asynchronous FIFO Thread pool + **Actor-like sequential command queue** per connection |
| **Protocol** | Full RESP (both multi-bulk arrays and inline) |
| **Storage** | Sharded `std::unordered_map` + `std::shared_mutex` (SWMR) |
| **TTL** | Min-heap (`std::priority_queue`) + background expiry thread (wait-on-earliest-deadline CV) |
| **Persistence** | AOF (Append-Only File) using absolute millisecond timestamps (`PXAT`/`PEXPIREAT`) |
| **Safety** | **Zero-data-race** architecture (reactor owns all connection memory; worker communications routed via stop_pipe wakeup) |
| **Testing** | Google Test unit tests + Python integration tests (pipelining, disconnects, restart recovery) |
| **Benchmark** | `redis-benchmark` compatible |

## 🚀 Supported Commands

```
PING [message]           — connectivity check
SET key value [EX sec]   — set key with optional TTL (persisted as PXAT)
GET key                  — get value
DEL key [key ...]        — delete one or more keys
EXPIRE key seconds       — set TTL on existing key (persisted as PEXPIREAT)
TTL key                  — remaining TTL (-1=no expiry, -2=not found)
INFO                     — server statistics
DBSIZE                   — number of live keys
COMMAND                  — stub (for redis-cli compatibility)
```

## 🏗 Architecture

```
redis_cpp/
├── src/
│   ├── net/
│   │   ├── Socket.hpp          ← RAII fd wrapper (move-only, auto-close)
│   │   ├── Connection.hpp      ← Per-conn: recv buf + mutex-protected send buf
│   │   ├── EpollReactor.hpp/cpp← epoll event loop (Reactor pattern)
│   ├── protocol/
│   │   ├── RespParser.hpp/cpp  ← RESP parser (multibulk + inline) + encoder
│   ├── store/
│   │   ├── KvStore.hpp/cpp     ← Sharded hash map, shared_mutex, lazy TTL
│   │   ├── TtlManager.hpp/cpp  ← Min-heap expiry, background thread
│   │   └── AofWriter.hpp/cpp   ← AOF persistence (RESP format), replay
│   ├── commands/
│   │   ├── Command.hpp         ← Abstract command interface
│   │   └── CommandDispatcher.cpp← GET/SET/DEL/EXPIRE/TTL/PING/INFO/...
│   ├── ThreadPool.hpp          ← std::thread pool, task queue, FIFO
│   └── main.cpp                ← CLI args, wiring, signal handlers
├── tests/
│   ├── test_kv_store.cpp
│   ├── test_resp_parser.cpp
│   ├── test_ttl_manager.cpp
│   └── test_command_dispatcher.cpp
└── benchmark/
    └── run_bench.sh            ← redis-benchmark automation
```

## 🔄 Request Flow

```
Client
  │
  ▼
EpollReactor (reactor thread)
  │  epoll_wait() / kevent() — read event fires
  │  → Drains socket recv buffer to connection's receive buffer
  │  → Splits stream into complete RESP frames
  │  → Enqueues commands to Connection's command queue
  │  → If not currently executing, pops first command and dispatches to ThreadPool
  │
  ▼
ThreadPool (worker thread N)
  │  Executes CommandDispatcher::dispatch(args, store)
  │  → Mutex-locked KvStore operations
  │  → Writes RESP reply to a safe pending-writes queue
  │  → Writes completion wakeups ('W' for write, 'C' for complete) to stop_pipe_
  │
  ▼
EpollReactor (reactor thread wakes up)
  │  → Wrote 'W' (write): flushes data to client (arms EPOLLOUT if partial)
  │  → Wrote 'C' (complete): sets connection processing = false, dispatches next command
  ▼
Client
```

## 🔧 Build

**Requirements:** Linux (epoll) or macOS (kqueue), GCC 11+ or Clang 13+, CMake 3.20+, internet (for fetching GoogleTest)

```bash
# Release build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# Debug build (with AddressSanitizer + UBSan)
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# Run server
./build/redis_cpp --port 6380 --workers 4
```

## 🧪 Tests

Both Google Test unit tests and Python integration tests are supported:

```bash
# Run Unit Tests
./build/run_tests

# Run Integration Tests (Pipelining, Disconnects, AOF Recovery)
python3 tests/integration_tests.py
```

## 📊 Benchmark

Ensure the server is running without AOF (`--no-aof`) for clean raw CPU/Memory results:

```bash
# Start server
./build/redis_cpp --port 6380 --no-aof

# Run benchmark suite
./benchmark/run_bench.sh 6380
```

### 📈 Typical Results (macOS, kqueue, 50 clients, 100k requests):
- **PING (Inline)**: ~30,000 RPS
- **SET**: ~24,500 RPS
- **GET**: ~25,400 RPS
- **SET with EX 10 (TTL)**: ~29,500 RPS
- **SET (Pipelined 16)**: **~81,300 RPS**
- **GET (Pipelined 16)**: **~87,400 RPS**

## ⚙️ CLI Options

| Option | Default | Description |
|--------|---------|-------------|
| `--port N` | 6380 | TCP listen port |
| `--workers N` | `nproc` | Worker thread count |
| `--idle-timeout N` | 60 | Connection idle timeout (seconds) |
| `--aof PATH` | `redis_cpp.aof` | AOF file path |
| `--no-aof` | — | Disable AOF persistence |

## 🔑 Design Decisions

### Actor-like FIFO Connection Queue
To ensure pipelined commands are executed and responded to in the exact order they are received on each socket connection, each `Connection` maintains an independent command queue. Commands are executed sequentially (one-by-one per connection) by the thread pool, ensuring 100% protocol FIFO order without sacrificing performance.

### Single-Threaded Connection Map Ownership
To prevent data races between worker threads and the reactor thread when adding/removing connections, the reactor thread holds exclusive ownership of the connection table. Worker threads communicate writing and completion events through a non-blocking pipe (`stop_pipe_`). The reactor thread wakes up, handles the events, and performs socket I/O in a single-threaded loop.

### Absolute Expire-At (PXAT/PEXPIREAT) Persistence
To prevent relative expiration drift and clock issues when replaying the AOF on startup, all relative TTL parameters (like `EX 10` or `expire key 10`) are logged in the AOF as absolute Unix epoch timestamps in milliseconds using `PXAT` and `PEXPIREAT`.

### Dynamic AOF Attachment (No Duplicates)
During startup replay, the active `AofWriter` is detached (`nullptr`) from `KvStore` so that replayed commands do not trigger new appends to the AOF. The `AofWriter` is dynamically attached to the store only after replay is completed, ensuring AOF file size does not duplicate on restart.

### Nested Stack Lifetime Shutdown
To prevent threads from accessing a partially destroyed reactor on graceful shutdown, the `ThreadPool` and `EpollReactor` are declared in nested stack lifetimes. During shutdown, the `ThreadPool` destructor executes first, blocking and joining all worker threads safely before the `EpollReactor` goes out of scope and frees its pipe and socket file descriptors.

## 📈 Upgrades vs. ev_kv_store (C)

| Feature | ev_kv_store (C) | redis_cpp (C++) |
|---------|----------------|-----------------|
| **Memory safety** | Manual malloc/free | RAII, move-only socket wrapper, smart pointers |
| **I/O multiplexing** | epoll only (Linux only) | Transparent epoll (Linux) + kqueue (macOS) |
| **Thread safety** | Global locks, data races | Sharded locks + Wakeup pipe event queue (Zero-data-race) |
| **Execution order** | Pipelined commands raced | Strict FIFO ordered connection command queue (Actor pattern) |
| **TTL mechanism** | Periodic linear O(N) scan | Min-heap O(log N) CV wait deadline scheduler |
| **AOF Recovery** | ❌ None | ✅ Absolute PXAT replay (no duplicate writes) |
| **Command design** | Switch/case dispatch | Command pattern (polymorphic) |
| **Kiểm thử** | ❌ Cầm tay / Sơ sài | ✅ 47 GTest unit tests + Python integration tests |
