#!/usr/bin/env python3
import subprocess
import socket
import time
import os
import sys

def main():
    port = 6389
    aof_file = "integration_test.aof"
    if os.path.exists(aof_file):
        os.remove(aof_file)

    print("════════════════════════════════════════════════════════")
    print("  redis_cpp Integration Tests")
    print("════════════════════════════════════════════════════════")

    # Start server in background
    print("[1/4] Starting server for AOF & Pipelining tests...")
    server = subprocess.Popen(["./build/redis_cpp", "--port", str(port), "--aof", aof_file])
    time.sleep(1.5) # Wait for startup

    try:
        # Test 1: Pipelining FIFO Order
        print("[2/4] Testing Pipelining FIFO response order...")
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", port))
        
        # Send 4 commands pipelined together
        pipeline_data = (
            b"*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
            b"*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\n2\r\n"
            b"*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
            b"*2\r\n$3\r\nGET\r\n$1\r\nb\r\n"
        )
        s.sendall(pipeline_data)

        # Read exact expected responses back
        expected = b"+OK\r\n+OK\r\n$1\r\n1\r\n$1\r\n2\r\n"
        res = b""
        s.settimeout(2.0)
        while len(res) < len(expected):
            chunk = s.recv(1024)
            if not chunk:
                break
            res += chunk

        assert res == expected, f"Pipelining failed. Expected: {expected!r}, Got: {res!r}"
        s.close()
        print("   ✓ Pipelining FIFO Order passed!")

        # Test 2: TTL and Restart Recovery (AOF)
        print("[3/4] Testing TTL setting and AOF recovery on restart...")
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", port))

        # Set key x with EX 10 (TTL of 10s)
        s.sendall(b"*5\r\n$3\r\nSET\r\n$1\r\nx\r\n$5\r\nhello\r\n$2\r\nEX\r\n$2\r\n10\r\n")
        assert s.recv(1024) == b"+OK\r\n"

        # Set key y without TTL
        s.sendall(b"*3\r\n$3\r\nSET\r\n$1\r\ny\r\n$5\r\nworld\r\n")
        assert s.recv(1024) == b"+OK\r\n"

        # Query TTL of key x, should be positive
        s.sendall(b"*2\r\n$3\r\nTTL\r\n$1\r\nx\r\n")
        ttl_res = s.recv(1024)
        assert ttl_res.startswith(b":"), f"Unexpected TTL reply: {ttl_res}"
        ttl_val = int(ttl_res[1:-2])
        assert 5 < ttl_val <= 10, f"TTL value out of expected range: {ttl_val}"
        s.close()

        # Shutdown server gracefully (flushes AOF)
        print("   Stopping server to save AOF...")
        server.terminate()
        server.wait()

        # Read baseline AOF size after flush
        with open(aof_file, "rb") as f:
            aof_content_before = f.read()

        # Restart server to replay AOF
        print("   Restarting server to replay AOF...")
        server = subprocess.Popen(["./build/redis_cpp", "--port", str(port), "--aof", aof_file])
        time.sleep(1.5)

        # Stop server again without any new write commands (should not append replay writes)
        server.terminate()
        server.wait()

        # Read AOF again; it should be identical (no re-logging during replay)
        with open(aof_file, "rb") as f:
            aof_content_after = f.read()

        assert aof_content_before == aof_content_after, f"AOF duplication detected! Before: {len(aof_content_before)} bytes, After: {len(aof_content_after)} bytes."
        print("   ✓ AOF duplication prevention verified!")

        # Restart server for verification checks
        server = subprocess.Popen(["./build/redis_cpp", "--port", str(port), "--aof", aof_file])
        time.sleep(1.5)

        # Connect and verify recovered state
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", port))

        # Get key x
        s.sendall(b"*2\r\n$3\r\nGET\r\n$1\r\nx\r\n")
        assert s.recv(1024) == b"$5\r\nhello\r\n", "Key 'x' not recovered correctly!"

        # Query TTL of x
        s.sendall(b"*2\r\n$3\r\nTTL\r\n$1\r\nx\r\n")
        ttl_res2 = s.recv(1024)
        ttl_val2 = int(ttl_res2[1:-2])
        assert 0 < ttl_val2 <= 10, f"TTL value after restart out of range: {ttl_val2}"

        # Get key y
        s.sendall(b"*2\r\n$3\r\nGET\r\n$1\r\ny\r\n")
        assert s.recv(1024) == b"$5\r\nworld\r\n", "Key 'y' not recovered correctly!"
        s.close()
        print("   ✓ TTL and AOF Restart recovery passed!")

    finally:
        server.terminate()
        server.wait()
        if os.path.exists(aof_file):
            os.remove(aof_file)

    # Test 3: Disconnect & Timeout
    print("[4/4] Testing connection idle timeout...")
    low_timeout_port = 6388
    low_timeout_server = subprocess.Popen([
        "./build/redis_cpp", 
        "--port", str(low_timeout_port), 
        "--idle-timeout", "2", 
        "--no-aof"
    ])
    time.sleep(1.5)

    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", low_timeout_port))
        
        # Wait 4 seconds (timeout is 2 seconds)
        time.sleep(4.0)

        # Try to read/write, should be closed by server
        s.settimeout(1.0)
        try:
            s.sendall(b"PING\r\n")
            res = s.recv(1024)
            assert len(res) == 0, f"Connection was not closed: {res!r}"
        except socket.timeout:
            raise AssertionError("Connection timed out on read, expected instant close notification")
        except OSError:
            pass # Socket closed locally/remotely is also expected
        
        s.close()
        print("   ✓ Idle timeout disconnect passed!")
    finally:
        low_timeout_server.terminate()
        low_timeout_server.wait()

    print("════════════════════════════════════════════════════════")
    print("★ ALL INTEGRATION TESTS PASSED SUCCESSFULLY! ★")
    print("════════════════════════════════════════════════════════")

if __name__ == "__main__":
    main()
