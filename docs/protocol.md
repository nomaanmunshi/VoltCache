# Wire protocol

VoltCache uses TCP, network byte order, and a length-prefixed list of byte strings. It does not implement Redis RESP.

## Frame layout

| Part           | Encoding                   | Meaning                                                 |
| -------------- | -------------------------- | ------------------------------------------------------- |
| Payload length | unsigned 32-bit big-endian | Number of bytes after this header; 2–65,536             |
| Field count    | unsigned 16-bit big-endian | 1–64 fields                                             |
| Field length   | unsigned 32-bit big-endian | Length of the next field                                |
| Field bytes    | raw bytes                  | May contain spaces, nulls, newlines, or non-UTF-8 bytes |

Each field has its own length. The decoded payload must end exactly after the final field. No trailing bytes or implicit terminators are allowed.

A PING request is 14 bytes:

```text
00 00 00 0a  00 01  00 00 00 04  50 49 4e 47
```

This is the actual hex encoding: a 10-byte payload, one field, a four-byte field length, then `PING`.

## Messages

Requests are lists such as `["SET", "key", "value"]`. The response is also a list:

| Situation        | Fields                                    |
| ---------------- | ----------------------------------------- |
| Successful write | `["OK"]`                                  |
| PING             | `["PONG"]`                                |
| GET hit          | `["VALUE", raw_value]`                    |
| GET miss         | `["NOT_FOUND"]`                           |
| Integer result   | `["INT", decimal_integer]`                |
| Statistics       | `["STATS", name, decimal_value, ...]`     |
| Error            | `["ERROR", code, human_readable_message]` |

Error codes include `COMMAND`, `ARITY`, `KEY`, `VALUE`, `SYNTAX`, `TTL`, `FRAME`, and `BUSY`. Error messages are explanatory text; clients should use the code rather than matching message wording.

A framing error closes the connection after attempting to send the error. Oversized advertised payloads are rejected before allocating their body. Invalid commands leave the connection open. A queue rejection returns BUSY without executing that command. At the connection limit, the server closes the newly accepted socket without allocating a session or promising a protocol response.

A client may send frames together. The server processes one command at a time per connection and sends replies in that connection's request order. Across separate connections, arrival and execution order are not guaranteed.

TCP failure after sending a command leaves its outcome uncertain: the command may already have executed. The protocol has no request IDs, cancellation acknowledgment, or exactly-once semantics. A disconnected client's admitted command can still run.

## Python example

The included client can send text or raw bytes:

```python
import sys
sys.path.insert(0, "demo")
from client import Client

with Client() as cache:
    print(cache.command("SET", "message", b"hello\x00world", "EX", "60"))
    print(cache.command("GET", "message"))
```

A `Client` is not thread-safe. Use a separate instance in each thread. Socket operations have timeouts, and the server also has full-frame deadlines. Unknown response framing is treated as an error.
