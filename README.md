# redis_cpp

> A Redis-compatible key-value server written in **C++20**, rebuilt from scratch with a focus on systems programming concepts and engineering best practices.

## ✨ Features

| Feature | Implementation |
|---------|---------------|
| **I/O Model** | Linux `epoll` edge-triggered + Reactor pattern |
| **Concurrency** | Thread pool (worker threads handle commands) |
| **Protocol** | Full RESP (both multi-bulk arrays and inline) |
| **Storage** | Sharded `std::unordered_map` + `std::shared_mutex` |
| **TTL** | Min-heap (`std::priority_queue`) + background expiry thread |
| **Persistence** | AOF (Append-Only File) + replay on startup |
| **Design** | RAII socket wrapper, Command pattern, SOLID principles |
| **Testing** | Google Test unit tests |
| **Benchmark** | `redis-benchmark` compatible |

## 🚀 Supported Commands

```
PING [message]           — connectivity check
SET key value [EX sec]   — set key with optional TTL
GET key                  — get value
DEL key [key ...]        — delete one or more keys
EXPIRE key seconds       — set TTL on existing key
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
  │  epoll_wait() — EPOLLIN fires
  │  → drain recv buffer (edge-triggered: loop until EAGAIN)
  │  → RespParser::parse() → extract complete RESP frames
  │  → for each frame: ThreadPool::enqueue(job)
  │
  ▼
ThreadPool (worker thread N)
  │  CommandDispatcher::dispatch(args, store)
  │  → KvStore::get/set/del/expire/...
  │  → returns RESP-encoded response string
  │  → reactor.send(fd, gen, resp)
  │
  ▼
EpollReactor (reactor thread)
  │  conn.send_enqueue(data)
  │  conn.flush(fd)
  │  → EAGAIN? → arm EPOLLOUT → flush on next writable event
  ▼
Client
```

## 🔧 Build

**Requirements:** Linux (epoll), GCC 11+ or Clang 13+, CMake 3.20+, internet (for fetching GoogleTest)

```bash
# Release build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Debug build (with AddressSanitizer + UBSan)
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc)

# Run server
./build/redis_cpp --port 6380 --workers 4

# Connect
redis-cli -p 6380
```

## 🧪 Tests

```bash
cmake -S . -B build-test
cmake --build build-test -j$(nproc)
cd build-test && ctest --output-on-failure
```

## 📊 Benchmark

```bash
# Start server
./build/redis_cpp --port 6380

# Run benchmark
chmod +x benchmark/run_bench.sh
./benchmark/run_bench.sh 6380
```

## ⚙️ CLI Options

| Option | Default | Description |
|--------|---------|-------------|
| `--port N` | 6380 | TCP listen port |
| `--workers N` | `nproc` | Worker thread count |
| `--idle-timeout N` | 60 | Connection idle timeout (seconds) |
| `--aof PATH` | `redis_cpp.aof` | AOF file path |
| `--no-aof` | — | Disable AOF persistence |

## 🔑 Design Decisions

### Reactor + Thread Pool separation
The reactor thread only does I/O (epoll_wait, read, write). Business logic runs in the pool. This prevents slow commands from blocking other connections.

### RAII everywhere
- `Socket`: auto-closes fd, move-only
- `KvStore`: RAII mutex locks via `std::unique_lock` / `std::shared_lock`
- `ThreadPool`: joins all threads in destructor
- `TtlManager`/`AofWriter`: stop background threads in destructor

### Lazy + Proactive TTL
- **Proactive**: `TtlManager` min-heap fires callback when key expires → immediately removed
- **Lazy**: `KvStore::get()` also checks expiry → defense-in-depth

### AOF Recovery
On startup, `AofWriter::replay()` re-executes all SET/DEL commands from the AOF file, rebuilding the dataset before accepting connections.

## 📈 Upgrades vs. ev_kv_store (C)

| Feature | ev_kv_store (C) | redis_cpp (C++) |
|---------|----------------|-----------------|
| Memory safety | Manual malloc/free | RAII, smart pointers |
| Protocol input | Custom line protocol | Full RESP (multibulk + inline) |
| TTL mechanism | Background scan (O(n)) | Min-heap (O(log n)) |
| Persistence | ❌ None | ✅ AOF + replay |
| Command design | Switch/case dispatch | Command pattern (polymorphic) |
| Type safety | Void pointers, casts | Templates, std::optional |
| Mutex type | `pthread_mutex_t` | `std::shared_mutex` (SWMR) |
