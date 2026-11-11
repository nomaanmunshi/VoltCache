#!/usr/bin/env python3
"""Black-box checks against real server, CLI, HTTP app, and SQLite processes."""
import argparse
import concurrent.futures
import http.client
import json
import os
import signal
import threading
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "demo"))
from client import CacheError, Client, encode, receive, stats

parser = argparse.ArgumentParser()
parser.add_argument("--server", required=True)
parser.add_argument("--cli", required=True)
ARGS, remaining = parser.parse_known_args()
ARGS.server = str(Path(ARGS.server).resolve())
ARGS.cli = str(Path(ARGS.cli).resolve())


class Process:
    def __init__(self, command):
        self.log = tempfile.TemporaryFile(mode="w+")
        self.child = subprocess.Popen(command, stdout=self.log, stderr=self.log, text=True)
        self.port = None
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.log.seek(0)
            lines = self.log.read()
            for line in lines.splitlines():
                if "listening on" in line:
                    self.port = int(line.split(":", 1)[1].split()[0])
                elif line.startswith("URL shortener:"):
                    self.port = int(line.rsplit(":", 1)[1])
            if self.port:
                return
            if self.child.poll() is not None:
                break
            time.sleep(0.02)
        self.stop()
        self.log.seek(0)
        raise RuntimeError("Process did not start: " + self.log.read())

    def stop(self):
        if self.child.poll() is None:
            self.child.terminate()
            try:
                self.child.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait()

    def close(self):
        self.stop()
        self.log.close()


def server(*args):
    return Process([ARGS.server, "--port", "0", *args])


class NetworkTests(unittest.TestCase):
    def setUp(self):
        self.server = server("--max-entries", "3", "--cleanup-ms", "20")
        self.addCleanup(self.server.close)

    def connect(self):
        return Client(port=self.server.port, timeout=2)

    def test_binary_fragmented_and_pipelined(self):
        with self.connect() as client:
            command = encode(["SET", "binary", b"hello\x00\n\xff world"])
            # Send header and body across arbitrary TCP writes.
            for part in [command[:1], command[1:3], command[3:7], command[7:]]:
                client.socket.sendall(part)
                time.sleep(0.003)
            self.assertEqual(receive(client.socket), [b"OK"])
            client.socket.sendall(encode(["GET", "binary"]) + encode(["PING"]) + encode(["GET", "missing"]))
            self.assertEqual(receive(client.socket), [b"VALUE", b"hello\x00\n\xff world"])
            self.assertEqual(receive(client.socket), [b"PONG"])
            self.assertEqual(receive(client.socket), [b"NOT_FOUND"])

    def test_malformed_and_oversized(self):
        for packet in [struct.pack("!I", 65537), b"\x00\x00\x00\x02\x00\x00",
                       b"\x00\x00\x00\x06\x00\x01\xff\xff\xff\xff"]:
            with self.connect() as client:
                client.socket.sendall(packet)
                self.assertEqual(receive(client.socket)[:2], [b"ERROR", b"FRAME"])
                self.assertEqual(client.socket.recv(1), b"")
        with self.connect() as client:
            self.assertEqual(client.command("PING"), [b"PONG"])
            with self.assertRaises(CacheError):
                client.command("GET")
            self.assertEqual(client.command("PING"), [b"PONG"])
            client.socket.sendall(b"\x00\x00") # Disconnect midway through header.

    def test_limits_lru_and_stats(self):
        with self.connect() as client:
            self.assertEqual(client.command("SET", "large", b"z" * 60000), [b"OK"])
            self.assertEqual(client.command("GET", "large"), [b"VALUE", b"z" * 60000])
            for key in ("a", "b", "c"):
                client.command("SET", key, key)
            client.command("GET", "a")
            client.command("SET", "d", "D")
            self.assertEqual(client.command("GET", "b"), [b"NOT_FOUND"])
            state = stats(client.command("STATS"))
            self.assertEqual(state["entries"], 3)
            self.assertEqual(state["evictions"], 2)
            self.assertGreaterEqual(state["hits"], 2)
            self.assertEqual(state["connected_clients"], 1)

    def test_background_expiration(self):
        with self.connect() as client:
            client.command("SET", "soon", "gone", "EX", "1")
            time.sleep(1.08)
            state = stats(client.command("STATS"))
            self.assertEqual(state["entries"], 0)
            self.assertEqual(state["expired_entries_removed"], 1)
            self.assertEqual(client.command("GET", "soon"), [b"NOT_FOUND"])

    def test_multiple_clients(self):
        def worker(index):
            with self.connect() as client:
                for _ in range(20):
                    self.assertEqual(client.command("PING"), [b"PONG"])
                    client.command("SET", str(index), "value")
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            list(pool.map(worker, range(12)))

    def test_cli_and_shutdown(self):
        result = subprocess.run([ARGS.cli, "--port", str(self.server.port)],
                                input='SET name "Nomaan Munshi"\nGET name\nQUIT\n',
                                text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"Nomaan Munshi"', result.stdout)
        result = subprocess.run([ARGS.cli, "--port", str(self.server.port), "NOPE"],
                                text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 1)
        self.server.stop()
        self.assertEqual(self.server.child.returncode, 0)

    def test_cli_deadline(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            finished = threading.Event()

            def silent_peer():
                connection, _ = listener.accept()
                with connection:
                    finished.wait(2)

            thread = threading.Thread(target=silent_peer)
            thread.start()
            try:
                result = subprocess.run([ARGS.cli, "--port", str(listener.getsockname()[1]),
                                         "--timeout-ms", "100", "PING"],
                                        capture_output=True, text=True, timeout=3)
                self.assertEqual(result.returncode, 1)
                self.assertIn("timed out", result.stderr)
            finally:
                finished.set()
                thread.join(timeout=3)

    def test_timeout_and_connection_limit(self):
        small = server("--max-clients", "1", "--timeout-ms", "150")
        self.addCleanup(small.close)
        with Client(port=small.port, timeout=1) as first:
            self.assertEqual(first.command("PING"), [b"PONG"])
            with socket.create_connection(("127.0.0.1", small.port), timeout=1) as second:
                self.assertEqual(second.recv(1), b"")
            # Idle frame deadline releases the first connection's slot.
            self.assertEqual(first.socket.recv(1), b"")
        with Client(port=small.port, timeout=1) as third:
            self.assertEqual(third.command("PING"), [b"PONG"])
            self.assertEqual(stats(third.command("STATS"))["rejected_clients"], 1)

    def test_overload_returns_busy(self):
        small = server("--queue-capacity", "1")
        self.addCleanup(small.close)
        clients = [Client(port=small.port, timeout=3) for _ in range(64)]
        self.addCleanup(lambda: [c.__exit__() for c in clients])
        busy = 0
        for _ in range(3):
            # Hold our test server while the burst reaches its sockets. This avoids
            # relying on the test client's speed to create queue pressure.
            os.kill(small.child.pid, signal.SIGSTOP)
            try:
                for client in clients:
                    client.socket.sendall(encode(["PING"]))
            finally:
                os.kill(small.child.pid, signal.SIGCONT)
            for client in clients:
                response = receive(client.socket)
                if response[:2] == [b"ERROR", b"BUSY"]:
                    busy += 1
                else:
                    self.assertEqual(response, [b"PONG"])
        self.assertGreater(busy, 0, "burst should saturate a one-command queue")
        state = stats(clients[0].command("STATS"))
        self.assertEqual(state["queue_peak"], 1)
        self.assertEqual(state["busy_responses"], busy)


class DemoTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.cache = server()
        self.addCleanup(self.cache.close)
        self.demo_command = [sys.executable, str(ROOT / "demo" / "app.py"), "--port", "0",
                             "--cache-port", str(self.cache.port), "--database",
                             str(Path(self.directory.name) / "links.sqlite3"), "--ttl", "1"]
        self.demo = Process(self.demo_command)
        self.addCleanup(self.demo.close)

    def request(self, method, path, data=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.demo.port, timeout=3)
        try:
            conn.request(method, path, json.dumps(data) if data is not None else None,
                         headers=headers or {"Content-Type": "application/json"})
            response = conn.getresponse()
            body = response.read()
            return response.status, dict(response.getheaders()), json.loads(body) if body and response.getheader("Content-Type", "").startswith("application/json") else body
        finally:
            conn.close()

    def test_cache_aside_expiry_fallback_and_persistence(self):
        status, _, link = self.request("POST", "/api/links", {"url": "https://example.com/path", "alias": "test-link"})
        self.assertEqual(status, 201)
        self.assertEqual(link["short_path"], "/s/test-link")
        self.assertEqual(self.request("GET", "/api/resolve/test-link")[2]["source"], "database")
        self.assertEqual(self.request("GET", "/api/resolve/test-link")[2]["source"], "cache")
        state = self.request("GET", "/api/stats")[2]
        self.assertEqual(state["database_reads"], 1)
        self.assertEqual(state["cache_hits"], 1)
        time.sleep(1.05)
        self.assertEqual(self.request("GET", "/api/resolve/test-link")[2]["source"], "database")
        status, headers, _ = self.request("GET", "/s/test-link")
        self.assertEqual(status, 302)
        self.assertEqual(headers["Location"], "https://example.com/path")
        self.assertEqual(headers["X-VoltCache-Source"], "cache")
        self.cache.stop()
        self.assertEqual(self.request("GET", "/api/resolve/test-link")[2]["source"], "database")
        state = self.request("GET", "/api/stats")[2]
        self.assertFalse(state["cache_available"])
        self.assertGreater(state["cache_errors"], 0)
        self.demo.close()
        self.demo = Process(self.demo_command)
        self.addCleanup(self.demo.close)
        self.assertEqual(self.request("GET", "/api/resolve/test-link")[2]["url"], "https://example.com/path")

    def test_validation_and_assets(self):
        for url in ["javascript:alert(1)", "https://x.test/\r\nX-Evil:yes", "https://user:pass@example.com", "https://", "ftp://example.com", None]:
            self.assertEqual(self.request("POST", "/api/links", {"url": url})[0], 400)
        self.assertEqual(self.request("POST", "/api/links", {"url": "https://example.com", "alias": "../oops"})[0], 400)
        self.assertEqual(self.request("POST", "/api/links", {"url": "https://example.com", "alias": "valid"})[0], 201)
        self.assertEqual(self.request("POST", "/api/links", {"url": "https://example.com", "alias": "valid"})[0], 409)
        self.assertEqual(self.request("POST", "/api/links", ["wrong-shape"])[0], 400)
        self.assertEqual(self.request("POST", "/api/links", {"url": "https://example.com"},
                                      {"Content-Type": "application/json", "Origin": "https://elsewhere.test"})[0], 403)
        self.assertEqual(self.request("GET", "/s/missing")[0], 404)
        self.assertEqual(self.request("GET", "/../app.py")[0], 404)
        for path in ["/", "/style.css", "/app.js"]:
            status, headers, body = self.request("GET", path)
            self.assertEqual(status, 200)
            self.assertGreater(len(body), 100)
            self.assertIn("Content-Security-Policy", headers)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
