"""Small synchronous VoltCache client. A connection can be reused within one thread."""
import socket
import struct
from contextlib import AbstractContextManager

MAX_FRAME = 65536


class CacheError(Exception):
    pass


def encode(fields):
    parts = [field.encode("utf-8") if isinstance(field, str) else field for field in fields]
    if not 1 <= len(parts) <= 64:
        raise ValueError("invalid field count")
    payload = struct.pack("!H", len(parts)) + b"".join(struct.pack("!I", len(p)) + p for p in parts)
    if len(payload) > MAX_FRAME:
        raise ValueError("frame too large")
    return struct.pack("!I", len(payload)) + payload


def receive(sock):
    def exact(size):
        data = bytearray()
        while len(data) < size:
            part = sock.recv(size - len(data))
            if not part:
                raise CacheError("cache disconnected")
            data.extend(part)
        return bytes(data)

    length, = struct.unpack("!I", exact(4))
    if not 2 <= length <= MAX_FRAME:
        raise CacheError("invalid response size")
    payload = exact(length)
    count, = struct.unpack_from("!H", payload)
    if not 1 <= count <= 64:
        raise CacheError("invalid response field count")
    pos, fields = 2, []
    for _ in range(count):
        if pos + 4 > len(payload):
            raise CacheError("truncated response")
        size, = struct.unpack_from("!I", payload, pos)
        pos += 4
        if size > len(payload) - pos:
            raise CacheError("truncated response field")
        fields.append(payload[pos:pos + size])
        pos += size
    if pos != len(payload):
        raise CacheError("trailing response bytes")
    return fields


class Client(AbstractContextManager):
    def __init__(self, host="127.0.0.1", port=6379, timeout=0.25):
        self.socket = socket.create_connection((host, port), timeout=timeout)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def command(self, *fields):
        self.socket.sendall(encode(fields))
        response = receive(self.socket)
        if response[0] == b"ERROR":
            raise CacheError(b": ".join(response[1:]).decode("utf-8", errors="replace"))
        return response

    def __exit__(self, *args):
        self.socket.close()


def stats(fields):
    if not fields or fields[0] != b"STATS" or len(fields) % 2 != 1:
        raise CacheError("unexpected STATS response")
    return {fields[i].decode("ascii"): int(fields[i + 1]) for i in range(1, len(fields), 2)}
