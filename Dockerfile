# ─── Stage 1: Build ───────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

# Install build dependencies (C++20 compiler, CMake, git/ca-certificates for GTest fetch)
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        cmake \
        make \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

# ─── Stage 2: Runtime ─────────────────────────────────────────────
FROM ubuntu:24.04

# Install redis-tools for redis-benchmark & redis-cli
RUN apt-get update && apt-get install -y --no-install-recommends \
        redis-tools \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/build/redis_cpp .
COPY --from=builder /app/build/run_tests .

EXPOSE 6380

ENTRYPOINT ["./redis_cpp"]
CMD ["--port", "6380", "--workers", "4", "--idle-timeout", "60"]
