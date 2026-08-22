#!/usr/bin/env python3

import os
import socket
import struct
import subprocess
import sys
import time


TAG_NIL = 0
TAG_ERROR = 1
TAG_STRING = 2
TAG_INTEGER = 3
TAG_DOUBLE = 4
TAG_ARRAY = 5


def packet(*arguments):
    encoded = [arg.encode() if isinstance(arg, str) else arg for arg in arguments]
    body = struct.pack("=I", len(encoded))
    for argument in encoded:
        body += struct.pack("=I", len(argument)) + argument
    return struct.pack("=I", len(body)) + body


def read_exact(connection, size):
    chunks = []
    while size:
        chunk = connection.recv(size)
        if not chunk:
            raise AssertionError("unexpected EOF")
        chunks.append(chunk)
        size -= len(chunk)
    return b"".join(chunks)


def decode_value(data, cursor=0):
    tag = data[cursor]
    cursor += 1
    if tag == TAG_NIL:
        return None, cursor
    if tag == TAG_ERROR:
        code, length = struct.unpack_from("=II", data, cursor)
        cursor += 8
        message = data[cursor : cursor + length].decode()
        return ("error", code, message), cursor + length
    if tag == TAG_STRING:
        (length,) = struct.unpack_from("=I", data, cursor)
        cursor += 4
        return data[cursor : cursor + length], cursor + length
    if tag == TAG_INTEGER:
        (value,) = struct.unpack_from("=q", data, cursor)
        return value, cursor + 8
    if tag == TAG_DOUBLE:
        (value,) = struct.unpack_from("=d", data, cursor)
        return value, cursor + 8
    if tag == TAG_ARRAY:
        (length,) = struct.unpack_from("=I", data, cursor)
        cursor += 4
        result = []
        for _ in range(length):
            value, cursor = decode_value(data, cursor)
            result.append(value)
        return result, cursor
    raise AssertionError(f"unknown response tag: {tag}")


def read_response(connection):
    (length,) = struct.unpack("=I", read_exact(connection, 4))
    body = read_exact(connection, length)
    result, cursor = decode_value(body)
    assert cursor == len(body)
    return result


def request(connection, *arguments):
    connection.sendall(packet(*arguments))
    return read_response(connection)


def unused_port():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def connect_with_retry(port):
    deadline = time.monotonic() + 5
    while True:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=1)
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.02)


def main():
    server_path = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./server")
    port = unused_port()
    process = subprocess.Popen(
        [server_path, str(port)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    connection = None
    try:
        connection = connect_with_retry(port)
        connection.settimeout(2)

        assert request(connection, "get", "missing") is None
        assert request(connection, "set", "hello", "world") is None
        assert request(connection, "get", "hello") == b"world"
        assert request(connection, "dbsize") == 1
        assert request(connection, "keys") == [b"hello"]

        # Multiple messages in one write verify pipelined framing.
        connection.sendall(packet("set", "pipe", "line") + packet("get", "pipe"))
        assert read_response(connection) is None
        assert read_response(connection) == b"line"

        assert request(connection, "zadd", "ranking", "1.5", "alice") == 1
        assert request(connection, "zadd", "ranking", "2", "bob") == 1
        assert request(connection, "zadd", "ranking", "2", "carol") == 1
        assert request(connection, "zadd", "ranking", "3", "alice") == 0
        assert request(connection, "zscore", "ranking", "alice") == 3.0
        assert request(connection, "zquery", "ranking", "0", "", "0", "10") == [
            b"bob",
            2.0,
            b"carol",
            2.0,
            b"alice",
            3.0,
        ]
        assert request(connection, "zquery", "ranking", "0", "", "1", "1") == [
            b"carol",
            2.0,
        ]
        assert request(connection, "zrem", "ranking", "carol") == 1

        type_error = request(connection, "set", "ranking", "wrong type")
        assert type_error[:2] == ("error", 3)
        argument_error = request(connection, "pexpire", "hello", "not-an-int")
        assert argument_error[:2] == ("error", 4)

        assert request(connection, "pexpire", "hello", "120") == 1
        remaining = request(connection, "pttl", "hello")
        assert 0 <= remaining <= 120
        time.sleep(0.16)
        assert request(connection, "get", "hello") is None
        assert request(connection, "pttl", "hello") == -2

        assert request(connection, "set", "permanent", "value") is None
        assert request(connection, "pexpire", "permanent", "1000") == 1
        assert request(connection, "persist", "permanent") == 1
        assert request(connection, "pttl", "permanent") == -1

        # Trigger progressive top-level rehashing.
        for index in range(300):
            assert request(connection, "set", f"key-{index}", str(index)) is None
        for index in range(300):
            assert request(connection, "get", f"key-{index}") == str(index).encode()

        # A >1000-node sorted set is destroyed by the worker pool.
        for index in range(1001):
            assert request(connection, "zadd", "large", str(index), f"item-{index}") == 1
        assert request(connection, "del", "large") == 1
        assert request(connection, "set", "after-unlink", "responsive") is None
        assert request(connection, "get", "after-unlink") == b"responsive"

        # The idle connection list closes inactive peers after five seconds.
        idle = connect_with_retry(port)
        idle.settimeout(7)
        time.sleep(5.3)
        assert idle.recv(1) == b""
        idle.close()

        print("server integration tests passed")
    finally:
        if connection is not None:
            connection.close()
        process.terminate()
        try:
            _, stderr = process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            _, stderr = process.communicate(timeout=3)
        if process.returncode not in (0, -15):
            sys.stderr.write(stderr)


if __name__ == "__main__":
    main()
