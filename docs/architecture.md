# Architecture and failure behavior

## Server ownership

One Asio event loop runs on one thread and owns all sockets, timers, the bounded FIFO command queue, and the cache. There is no worker pool and no mutex around cache state. More connections can make progress while another waits for network bytes, but CPU work is serialized.

Each session reads a four-byte header and then the exact payload length. It validates and decodes the payload, submits one command, writes the complete response, and starts reading again. `shared_ptr` keeps a session alive while asynchronous operations use its buffers. A queued reply callback holds a weak reference so the queue does not retain dead sessions unnecessarily.

The queue drains one command per posted handler, yielding to other ready handlers between commands. The application queue is count-bounded, frames are size-bounded, and connections are count-bounded. This bounds application allocations by configuration, not by a precise byte budget. Kernel socket buffers remain outside that accounting.

Default maximum buffered payload bytes across 256 clients can be around 16 MiB before other overhead. Pending command strings and response buffers need additional memory. Cache values dominate memory at high capacities.

The server stops accepting, cancels cleanup, and closes active sessions on SIGINT or SIGTERM. Queued commands may be discarded during shutdown; there is no persistence guarantee.

## LRU and expiration

The hash map stores each value, an optional monotonic expiration deadline, and an iterator into the recency list. `GET` and `SET` move the list node to the front. `EXISTS`, `TTL`, and `EXPIRE` do not change recency. Removal updates both containers.

Lookup is average O(1), and list movement is O(1). Keys and values still need to be copied or hashed. A periodic sweep traverses all entries, O(n), on the event loop. Admission at capacity also sweeps before choosing an LRU victim. A full cache with no expired entries therefore has an O(n) insertion path. This is a simple, honest first implementation; an expiration index would be justified if measurements showed this scan to be a bottleneck.

TTL uses `steady_clock`, so wall-clock corrections do not extend an entry's life. At exactly the deadline the entry is absent, even before cleanup runs. `TTL` floors remaining seconds, so a live entry with less than one second left reports zero.

`SET` removes the previous TTL. `SET ... EX seconds` validates all arguments first and applies value and deadline within one event-loop command. This avoids a permanent entry if a client disconnects between separate SET and EXPIRE calls.

Statistics count GET hits and misses only. Removing an expired entry increments the expired counter once. Explicit DEL or EXPIRE-zero deletion is not an LRU eviction or an automatic expiration.

## Cache-aside URL resolution

SQLite is the source of truth. Links are immutable and the app uses parameterized SQL. Creation writes only SQLite, so the first resolution can demonstrate a miss. Existing cache copies live for 60 seconds by default.

Resolution does this:

1. Open a cache connection and GET `shortlink:<code>`.
2. Return a valid cached destination on a hit.
3. On miss, connection failure, timeout, BUSY, or invalid response, read SQLite.
4. If there is a destination and a usable cache connection, attempt SET with an atomic expiration.
5. Return the destination even if population fails.

There is no negative caching. Repeated unknown codes reach SQLite. There is no single-flight suppression, so simultaneous misses can repeat a read. These are acceptable limits of the small demo.

Cache entries are trusted as local application data, but cached destinations are still checked for an HTTP(S) scheme and header-unsafe characters before use. Another local cache client could intentionally overwrite a short-link key; there are no per-client permissions or namespaces enforced by the server.

The Python HTTP app binds only to loopback, limits request bodies, caps request threads at 32, applies socket timeouts, and serves a fixed asset allowlist. It is not an internet-facing web service. Generated links use random codes or a validated custom code. A short link redirects with HTTP 302 and `Cache-Control: no-store`, so browser redirect caching does not conceal cache behavior during the demo.

## Dashboard

The frontend has no package dependencies or external fonts. It uses the same-origin HTTP API, constructs dynamic DOM nodes with `textContent`, and only turns validated destinations into links. The content security policy blocks external scripts and framing.

The animated hero is decorative. Request path highlights follow actual API results. The browser polls statistics every five seconds while the tab is visible. Displayed hit rate uses successful cache GET attempts made by the demo; network failures are separate from misses. Entries are server-wide. No synthetic latency chart or claimed speedup is displayed.

## What to improve after measuring

- A tracked byte budget if entry count is too coarse for the intended workload.
- An expiration index or bounded incremental sweep if O(n) cleanup delays requests.
- Connection pooling in the demo if connection setup becomes a measured cost.
- Request coalescing if concurrent misses overload the source of truth.
- Authentication, TLS, and abuse prevention before any public deployment.

Persistence, replication, and distributed consistency would materially change this project's scope and are not implied by the current implementation.
