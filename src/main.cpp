/*
 * main.cpp — Entry point for redis_cpp server.
 *
 * Wires together:
 *   AofWriter → KvStore (replay AOF on startup)
 *   ThreadPool → EpollReactor
 *   CommandDispatcher → per-request dispatch
 *   Signal handler → graceful shutdown
 *
 * Usage:
 *   ./redis_cpp [--port N] [--workers N] [--aof PATH]
 *               [--idle-timeout N] [--no-aof]
 */
#include <csignal>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <string>

#include "net/EpollReactor.hpp"
#include "store/KvStore.hpp"
#include "store/AofWriter.hpp"
#include "store/TtlManager.hpp"
#include "commands/CommandDispatcher.hpp"
#include "protocol/RespParser.hpp"
#include "ThreadPool.hpp"

// ─── Defaults ────────────────────────────────────────────────────────────────
static constexpr uint16_t DEFAULT_PORT    = 6380;
static constexpr int      DEFAULT_WORKERS = 0;     // 0 = hardware_concurrency
static constexpr int      DEFAULT_IDLE_S  = 60;
static const std::string  DEFAULT_AOF     = "redis_cpp.aof";

// ─── Global reactor for signal handler ───────────────────────────────────────
static net::EpollReactor* g_reactor = nullptr;

static void sig_handler(int) {
    if (g_reactor) g_reactor->stop();
}

// ─────────────────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [OPTIONS]\n\n"
        << "Options:\n"
        << "  --port N         Listen port        (default: " << DEFAULT_PORT << ")\n"
        << "  --workers N      Worker threads      (default: auto)\n"
        << "  --idle-timeout N Connection timeout  (default: " << DEFAULT_IDLE_S << "s)\n"
        << "  --aof PATH       AOF file path       (default: " << DEFAULT_AOF << ")\n"
        << "  --no-aof         Disable persistence\n"
        << "  --help           Show this help\n\n"
        << "Commands supported: GET SET DEL EXPIRE TTL PING INFO DBSIZE COMMAND\n";
}

int main(int argc, char* argv[]) {
    uint16_t    port         = DEFAULT_PORT;
    int         workers      = DEFAULT_WORKERS;
    int         idle_timeout = DEFAULT_IDLE_S;
    std::string aof_path     = DEFAULT_AOF;
    bool        use_aof      = true;

    static struct option long_opts[] = {
        {"port",         required_argument, nullptr, 'p'},
        {"workers",      required_argument, nullptr, 'w'},
        {"idle-timeout", required_argument, nullptr, 'i'},
        {"aof",          required_argument, nullptr, 'a'},
        {"no-aof",       no_argument,       nullptr, 'n'},
        {"help",         no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:w:i:a:nh", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'p': port         = static_cast<uint16_t>(std::stoi(optarg)); break;
        case 'w': workers      = std::stoi(optarg);                        break;
        case 'i': idle_timeout = std::stoi(optarg);                        break;
        case 'a': aof_path     = optarg;                                   break;
        case 'n': use_aof      = false;                                    break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    // ── AOF Writer ────────────────────────────────────────────────────
    std::unique_ptr<store::AofWriter> aof;
    if (use_aof) {
        aof = std::make_unique<store::AofWriter>(aof_path, store::AofSync::EVERY_SECOND);
        if (!aof->is_open()) {
            std::cerr << "[main] Warning: Could not open AOF file " << aof_path
                      << " — running without persistence.\n";
            aof.reset();
        }
    }

    // ── KV Store ──────────────────────────────────────────────────────
    auto store = std::make_unique<store::KvStore>(
        store::KvStore::DEFAULT_SHARDS, aof.get(), /*enable_ttl=*/true);

    // ── Replay AOF (restore state from disk) ──────────────────────────
    if (use_aof) {
        store::AofWriter tmp("", store::AofSync::NEVER);
        tmp.replay(
            aof_path,
            [&](std::string_view k, std::string_view v) {
                return store->set(k, v);
            },
            [&](std::string_view k, std::string_view v, int64_t expire_at_ms) {
                int64_t now = store::KvStore::epoch_ms();
                if (expire_at_ms <= now) return false;
                int64_t remaining_s = (expire_at_ms - now) / 1000;
                if (remaining_s <= 0) remaining_s = 1;
                return store->set_ex(k, v, remaining_s);
            },
            [&](std::string_view k) {
                return store->del(k);
            },
            [&](std::string_view k, int64_t expire_at_ms) {
                int64_t now = store::KvStore::epoch_ms();
                if (expire_at_ms <= now) {
                    store->del(k);
                    return true;
                }
                int64_t remaining_s = (expire_at_ms - now) / 1000;
                if (remaining_s <= 0) remaining_s = 1;
                return store->expire(k, remaining_s);
            }
        );
    }

    // ── Command Dispatcher ────────────────────────────────────────────
    auto dispatcher = std::make_unique<commands::CommandDispatcher>();

    // ── Thread Pool ───────────────────────────────────────────────────
    size_t n_threads = workers > 0
                       ? static_cast<size_t>(workers)
                       : std::thread::hardware_concurrency();
    ThreadPool pool(n_threads);

    std::cout << "[main] Thread pool: " << n_threads << " workers\n";

    // ── Epoll Reactor ─────────────────────────────────────────────────
    net::ReactorConfig cfg;
    cfg.port           = port;
    cfg.idle_timeout_s = idle_timeout;
    cfg.on_message = [&](int fd, uint64_t gen, std::vector<std::string> args) {
        // Normalize command name to uppercase
        if (!args.empty()) {
            for (char& c : args[0])
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        std::string resp = dispatcher->dispatch(args, *store);
        // reactor.send is thread-safe
        // We capture g_reactor; it's set before run()
        g_reactor->send(fd, gen, resp);
    };

    net::EpollReactor reactor(cfg, pool);
    g_reactor = &reactor;

    // ── Signals ───────────────────────────────────────────────────────
    ::signal(SIGINT,  sig_handler);
    ::signal(SIGTERM, sig_handler);

    std::cout << "[main] redis_cpp ready on port " << port
              << " (AOF: " << (use_aof ? aof_path : "disabled") << ")\n"
              << "  redis-cli -p " << port << "\n";

    // ── Event loop (blocking) ─────────────────────────────────────────
    reactor.run();

    // ── Graceful shutdown ─────────────────────────────────────────────
    g_reactor = nullptr;
    std::cout << "[main] Shutting down. Bye.\n";
    return 0;
}
