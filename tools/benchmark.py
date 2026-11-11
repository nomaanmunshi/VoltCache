#!/usr/bin/env python3
"""Measure sequential GET round trips over persistent TCP connections."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
import platform
import random
import secrets
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "demo"))
from client import CacheError, Client, stats


def percentile(samples, fraction):
    return samples[max(0, math.ceil(len(samples) * fraction) - 1)] if samples else None


def cpu_model():
    if sys.platform.startswith("linux"):
        try:
            for line in Path("/proc/cpuinfo").read_text().splitlines():
                if line.startswith("model name"):
                    return line.partition(":")[2].strip()
        except OSError:
            pass
    return platform.processor() or "unknown"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6379)
    parser.add_argument("--clients", type=int, default=4)
    parser.add_argument("--requests", type=int, default=5000, help="requests per client")
    parser.add_argument("--keys", type=int, default=100)
    parser.add_argument("--value-size", type=int, default=128)
    parser.add_argument("--hit-ratio", type=float, default=0.8)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.clients <= 256 or not 1 <= args.requests or args.clients * args.requests > 1000000:
        parser.error("use 1–256 clients, positive requests, and at most 1,000,000 total requests")
    if not 1 <= args.keys <= 10000 or not 0 <= args.value_size <= 60000 or not 0 <= args.hit_ratio <= 1:
        parser.error("keys, value-size, or hit-ratio out of range")
    namespace = "bench:" + secrets.token_hex(4) + ":"
    keys = [namespace + str(i) for i in range(args.keys)]
    value = b"x" * args.value_size
    barrier = threading.Barrier(args.clients + 1)
    prepared = []
    before = None
    try:
        with Client(args.host, args.port, timeout=5) as control:
            state = stats(control.command("STATS"))
            if state["capacity"] < args.keys:
                parser.error("cache capacity is smaller than --keys")
            for key in keys:
                control.command("SET", key, value)
                prepared.append(key)
            before = stats(control.command("STATS"))

        def worker(index):
            rng = random.Random(1729 + index)
            latencies = []
            hits = misses = errors = 0
            client = None
            try:
                client = Client(args.host, args.port, timeout=5)
            except (OSError, CacheError):
                pass
            barrier.wait()
            if client is None:
                return [], 0, 0, args.requests
            try:
                for _ in range(args.requests):
                    key = rng.choice(keys) if rng.random() < args.hit_ratio else namespace + "missing"
                    start = time.perf_counter_ns()
                    try:
                        reply = client.command("GET", key)
                        if len(reply) == 2 and reply[0] == b"VALUE":
                            if reply[1] != value:
                                raise CacheError("value mismatch")
                            hits += 1
                        elif reply == [b"NOT_FOUND"]:
                            misses += 1
                        else:
                            raise CacheError("unexpected response")
                        latencies.append((time.perf_counter_ns() - start) / 1000)
                    except (OSError, CacheError):
                        errors += 1
            finally:
                client.__exit__()
            return latencies, hits, misses, errors

        with ThreadPoolExecutor(max_workers=args.clients) as pool:
            futures = [pool.submit(worker, i) for i in range(args.clients)]
            start = time.perf_counter()
            barrier.wait()
            results = [future.result() for future in futures]
            elapsed = time.perf_counter() - start
        samples = sorted(value for result in results for value in result[0])
        hits = sum(result[1] for result in results)
        misses = sum(result[2] for result in results)
        errors = sum(result[3] for result in results)
        with Client(args.host, args.port, timeout=5) as control:
            after = stats(control.command("STATS"))
        report = dict(
            timestamp=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            environment=dict(os=platform.platform(), machine=platform.machine(), processor=cpu_model(),
                             logical_cpus=os.cpu_count(), python=platform.python_version()),
            workload=dict(host=args.host, port=args.port, clients=args.clients, requests_per_client=args.requests,
                          keys=args.keys, value_bytes=args.value_size, requested_hit_ratio=args.hit_ratio,
                          connection="persistent, one outstanding GET per client", seed=1729),
            results=dict(elapsed_seconds=round(elapsed, 6), successes=hits + misses, errors=errors,
                         hits=hits, misses=misses, successful_requests_per_second=round((hits + misses) / elapsed, 2),
                         latency_microseconds=dict(p50=percentile(samples, .5), p95=percentile(samples, .95),
                                                   p99=percentile(samples, .99))),
            server_before=before, server_after=after)
        text = json.dumps(report, indent=2)
        print(text)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text + "\n")
        return 1 if errors else 0
    finally:
        if prepared:
            try:
                with Client(args.host, args.port, timeout=5) as control:
                    for key in prepared:
                        control.command("DEL", key)
            except (OSError, CacheError):
                print("Warning: benchmark keys could not be removed.", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
