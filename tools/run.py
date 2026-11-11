#!/usr/bin/env python3
"""Build VoltCache and run both the cache and local demo. Ctrl+C stops both."""
import argparse
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "demo"))
from client import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--cache-port", type=int, default=6379)
    parser.add_argument("--ttl", type=int, default=60)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 1 <= args.cache_port <= 65535:
        parser.error("ports must be between 1 and 65535")
    if not 1 <= args.ttl <= 31536000:
        parser.error("TTL must be between 1 and 31536000")
    if not args.skip_build:
        if not shutil.which("cmake"):
            parser.error("CMake is missing. On macOS: brew install cmake")
        subprocess.run(["cmake", "-S", str(ROOT), "-B", str(ROOT / "build"),
                        "-DCMAKE_BUILD_TYPE=Release"], check=True)
        subprocess.run(["cmake", "--build", str(ROOT / "build"), "--parallel", "2"], check=True)
    binary = ROOT / "build" / "voltcache-server"
    if not binary.is_file():
        parser.error("build/voltcache-server is missing; run without --skip-build")
    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    processes = []
    try:
        cache = subprocess.Popen([str(binary), "--port", str(args.cache_port)])
        processes.append(cache)
        deadline = time.monotonic() + 5
        while True:
            if cache.poll() is not None:
                raise RuntimeError("cache server exited; check whether its port is already in use")
            try:
                with Client(port=args.cache_port) as client:
                    if client.command("PING") == [b"PONG"]:
                        break
            except OSError:
                pass
            if time.monotonic() > deadline:
                raise RuntimeError("cache server did not become ready")
            stop.wait(0.05)
        demo = subprocess.Popen([sys.executable, str(ROOT / "demo" / "app.py"),
                                 "--port", str(args.port), "--cache-port", str(args.cache_port),
                                 "--ttl", str(args.ttl)])
        processes.append(demo)
        print(f"\nOpen http://127.0.0.1:{args.port} — Ctrl+C stops both services.\n", flush=True)
        while not stop.wait(0.2):
            for process in processes:
                if process.poll() is not None:
                    raise RuntimeError("a service exited; see its output above")
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
        for process in processes:
            try:
                process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"run: {error}", file=sys.stderr)
        sys.exit(1)
