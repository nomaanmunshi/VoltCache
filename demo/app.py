#!/usr/bin/env python3
"""Local URL shortener: SQLite owns links; VoltCache holds temporary copies."""
import argparse
import json
import re
import secrets
import signal
import sqlite3
import threading
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

from client import CacheError, Client, stats

STATIC = Path(__file__).parent / "static"
CODE = re.compile(r"[A-Za-z0-9_-]{3,32}\Z")
CACHE_FAILURE = (OSError, CacheError, ValueError)


def validate_url(value):
    if not isinstance(value, str) or not 1 <= len(value) <= 2048:
        raise ValueError("Enter a URL of up to 2048 characters.")
    if any(c.isspace() or ord(c) < 32 or ord(c) == 127 for c in value) or "\\" in value:
        raise ValueError("URLs cannot contain whitespace, control characters, or backslashes.")
    try:
        parsed = urlsplit(value)
        host = parsed.hostname
        port = parsed.port
        if parsed.scheme not in ("http", "https") or not host or parsed.username is not None or parsed.password is not None:
            raise ValueError
        # Use ASCII URLs; browsers can supply international domains in punycode.
        value.encode("ascii")
        if port is not None and not 1 <= port <= 65535:
            raise ValueError
        if any(c in host for c in '<>"\''):
            raise ValueError
    except ValueError:
        raise ValueError("Use a valid http:// or https:// URL without a username or password.") from None
    return value


class Store:
    def __init__(self, path, cache_host, cache_port, ttl, cache_timeout):
        self.path = str(path)
        self.cache_host, self.cache_port = cache_host, cache_port
        self.ttl, self.cache_timeout = ttl, cache_timeout
        self.lock = threading.Lock()
        self.counters = dict(cache_hits=0, cache_misses=0, database_reads=0, cache_errors=0)
        with self.connect() as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("CREATE TABLE IF NOT EXISTS links (code TEXT PRIMARY KEY, url TEXT NOT NULL, "
                       "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')))")

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=3)
        try:
            with db:
                yield db
        finally:
            db.close()

    def cache(self):
        return Client(self.cache_host, self.cache_port, self.cache_timeout)

    def count(self, name):
        with self.lock:
            self.counters[name] += 1

    def create(self, url, alias=None):
        url = validate_url(url)
        if alias is not None and (not isinstance(alias, str) or not CODE.fullmatch(alias)):
            raise ValueError("Custom code must be 3–32 letters, numbers, hyphens, or underscores.")
        for _ in range(5):
            code = alias or secrets.token_urlsafe(6)
            try:
                with self.connect() as db:
                    db.execute("INSERT INTO links(code, url) VALUES (?, ?)", (code, url))
                return dict(code=code, url=url, short_path="/s/" + code)
            except sqlite3.IntegrityError:
                if alias:
                    raise FileExistsError("That code is already taken.") from None
        raise RuntimeError("Could not generate a unique code; try again.")

    def resolve(self, code):
        if not CODE.fullmatch(code):
            return None, "missing"
        key = "shortlink:" + code
        client = None
        try:
            try:
                client = self.cache()
                response = client.command("GET", key)
                if len(response) == 2 and response[0] == b"VALUE":
                    url = validate_url(response[1].decode("utf-8"))
                    self.count("cache_hits")
                    return url, "cache"
                if response != [b"NOT_FOUND"]:
                    raise CacheError("unexpected GET response")
                self.count("cache_misses")
            except CACHE_FAILURE:
                self.count("cache_errors")
                if client:
                    client.__exit__()
                    client = None
            self.count("database_reads")
            with self.connect() as db:
                row = db.execute("SELECT url FROM links WHERE code = ?", (code,)).fetchone()
            if row is None:
                return None, "missing"
            if client:
                try:
                    if client.command("SET", key, row[0], "EX", str(self.ttl)) != [b"OK"]:
                        raise CacheError("unexpected SET response")
                except CACHE_FAILURE:
                    self.count("cache_errors")
            return row[0], "database"
        finally:
            if client:
                client.__exit__()

    def recent(self):
        with self.connect() as db:
            rows = db.execute("SELECT code, url, created_at FROM links ORDER BY rowid DESC LIMIT 12").fetchall()
        return [dict(code=r[0], url=r[1], created_at=r[2], short_path="/s/" + r[0]) for r in rows]

    def statistics(self):
        with self.lock:
            result = dict(self.counters)
        result.update(cache_available=False, cache=None, ttl=self.ttl)
        try:
            with self.cache() as client:
                result.update(cache_available=True, cache=stats(client.command("STATS")))
        except CACHE_FAILURE:
            pass
        return result


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "VoltCacheDemo/1.0"

    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def respond(self, status, body=b"", content_type="application/json; charset=utf-8", **headers):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; "
                         "img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
        for name, value in headers.items():
            self.send_header(name.replace("_", "-"), value)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def json(self, status, value):
        self.respond(status, json.dumps(value).encode())

    def do_GET(self):
        try:
            path = urlsplit(self.path).path
            assets = {"/": ("index.html", "text/html; charset=utf-8"),
                      "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                      "/style.css": ("style.css", "text/css; charset=utf-8")}
            if path in assets:
                filename, mime = assets[path]
                return self.respond(200, (STATIC / filename).read_bytes(), mime)
            if path == "/api/stats":
                return self.json(200, self.server.store.statistics())
            if path == "/api/links":
                return self.json(200, self.server.store.recent())
            if path.startswith("/s/") or path.startswith("/api/resolve/"):
                code = path.rsplit("/", 1)[-1]
                url, source = self.server.store.resolve(code)
                if url is None:
                    return self.json(404, dict(error="Short link not found."))
                if path.startswith("/s/"):
                    return self.respond(302, Location=url, X_VoltCache_Source=source)
                return self.json(200, dict(code=code, url=url, source=source))
            self.json(404, dict(error="Not found."))
        except (BrokenPipeError, ConnectionResetError):
            pass
        except sqlite3.Error:
            self.json(503, dict(error="Database is temporarily unavailable."))

    def do_POST(self):
        # The local demo does not expose a cross-origin API.
        origin = self.headers.get("Origin")
        if origin and origin != "http://" + self.headers.get("Host", ""):
            self.close_connection = True
            return self.json(403, dict(error="Cross-origin requests are not allowed."))
        if urlsplit(self.path).path != "/api/links":
            self.close_connection = True
            return self.json(404, dict(error="Not found."))
        try:
            if self.headers.get("Transfer-Encoding") or self.headers.get_content_type() != "application/json":
                raise ValueError("Send a JSON request with Content-Length.")
            length = int(self.headers.get("Content-Length", "0"))
            if not 1 <= length <= 8192:
                raise ValueError("Request body must be 1–8192 bytes.")
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError("Incomplete request body.")
            data = json.loads(raw)
            if not isinstance(data, dict):
                raise ValueError("Request must be a JSON object.")
            result = self.server.store.create(data.get("url"), data.get("alias") or None)
            self.json(201, result)
        except ValueError as error:
            self.close_connection = True
            self.json(400, dict(error=str(error)))
        except FileExistsError as error:
            self.json(409, dict(error=str(error)))
        except sqlite3.Error:
            self.json(503, dict(error="Database is temporarily unavailable."))



class DemoServer(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False
    allow_reuse_address = True

    def __init__(self, address, store):
        self.store = store
        self.slots = threading.BoundedSemaphore(32)
        super().__init__(address, Handler)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--database", type=Path, default=Path(__file__).parent / "data" / "links.sqlite3")
    parser.add_argument("--cache-host", default="127.0.0.1")
    parser.add_argument("--cache-port", type=int, default=6379)
    parser.add_argument("--ttl", type=int, default=60)
    parser.add_argument("--cache-timeout", type=float, default=0.25)
    args = parser.parse_args()
    if not 0 <= args.port <= 65535 or not 1 <= args.cache_port <= 65535:
        parser.error("port out of range")
    if not 1 <= args.ttl <= 31536000 or not 0 < args.cache_timeout <= 5:
        parser.error("TTL or timeout out of range")
    args.database.parent.mkdir(parents=True, exist_ok=True)
    store = Store(args.database, args.cache_host, args.cache_port, args.ttl, args.cache_timeout)
    with DemoServer(("127.0.0.1", args.port), store) as server:
        def stop(_signal, _frame):
            raise KeyboardInterrupt

        signal.signal(signal.SIGTERM, stop)
        print(f"URL shortener: http://127.0.0.1:{server.server_port}", flush=True)
        try:
            server.serve_forever(poll_interval=0.2)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
