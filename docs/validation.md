# Validation record

Development validation on 11 November 2026:

- Release build: GCC 14.2.0, C++17, CMake; no compiler warnings emitted by the configured warning flags.
- C++ unit executable: 63 checks passed, covering cache policy, expiration boundaries, protocol validation, command behavior, and bounded queue admission.
- Integration suite: 11 process-level tests passed against the actual cache server, CLI, HTTP app, and temporary SQLite database.
- AddressSanitizer and UndefinedBehaviorSanitizer: the unit and integration suites passed with `ASAN_OPTIONS=detect_leaks=0`. LeakSanitizer could not operate under this environment's process restrictions, so leaks were not verified.
- Browser smoke test: Chromium headless, real local backend. Created a link, checked a first database lookup and subsequent cache hit, stopped the cache, and verified fallback. Live counters updated, JavaScript emitted no page errors, layouts did not overflow at 390, 700, or 1024 pixels, and reduced motion disabled the cache-stack animation. Desktop layout was also inspected at 1440 pixels.
- Launcher: started both services, served the stats endpoint, and stopped both on termination.
- Benchmark: 8,000 GETs, 4 persistent clients, 128-byte values, 100 preloaded keys, target 80% hit ratio, zero errors. See `benchmark-sample.json` for raw latency percentiles, actual hit ratio, throughput, and environment.

This is evidence from one Linux development environment. The macOS workflow is included but has not been run here. The benchmark is a short shared-container measurement, not a production load test or a comparison with Redis or SQLite.

Reproduce the core checks with `ctest --test-dir build --output-on-failure`. The README describes optional sanitizers, browser checks, and benchmark commands.
