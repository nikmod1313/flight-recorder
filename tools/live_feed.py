#!/usr/bin/env python3
"""Streams public Binance trades and best quotes to stdout, then "end".

Each line is "price qty side bid ask bid_qty ask_qty". side is +1 if a buyer started the trade,
-1 if a seller did, and 0 for a book update, which has no price or qty.
bid_qty and ask_qty are how much is waiting at the best bid and ask.
Numbers are scaled by 1e8 into exact integers. Uses only the standard library.
No account or key is needed: this is public market data.

  python3 tools/live_feed.py --symbol btcusdt --seconds 60 | build/fr_tool record-stdin clean run.log
"""
import argparse
import base64
import json
import os
import socket
import ssl
import struct
import sys
import time
from decimal import Decimal

HOST = "data-stream.binance.vision"
PORT = 443
SCALE = Decimal(10) ** 8
CA_FILE = "/etc/ssl/cert.pem" if os.path.exists("/etc/ssl/cert.pem") else None  # python.org builds ship none


class FeedError(Exception):
    pass


def scaled(text):
    return int(Decimal(text) * SCALE)


def read_exact(sock, count):
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise FeedError("connection closed by server")
        data += chunk
    return data


def connect(symbol):
    raw = socket.create_connection((HOST, PORT), timeout=15)
    sock = ssl.create_default_context(cafile=CA_FILE).wrap_socket(raw, server_hostname=HOST)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(
        (
            f"GET /stream?streams={symbol}@trade/{symbol}@bookTicker HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        ).encode()
    )
    reply = b""
    while b"\r\n\r\n" not in reply:
        reply += read_exact(sock, 1)
    status = reply.split(b"\r\n", 1)[0].decode(errors="replace")
    if " 101 " not in status + " ":
        raise FeedError(f"server refused the stream: {status}")
    return sock


def send_frame(sock, opcode, payload):
    mask = os.urandom(4)
    masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    sock.sendall(bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + masked)


def read_frame(sock):
    first, second = read_exact(sock, 2)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack(">H", read_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read_exact(sock, 8))[0]
    return first & 0x80, first & 0x0F, read_exact(sock, length)


def messages(sock):
    parts = b""
    while True:
        final, opcode, payload = read_frame(sock)
        if opcode == 0x9:
            send_frame(sock, 0xA, payload)
        elif opcode == 0x8:
            raise FeedError("server closed the stream")
        elif opcode in (0x0, 0x1, 0x2):
            parts += payload
            if final:
                yield parts
                parts = b""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--symbol", default="btcusdt")
    parser.add_argument("--seconds", type=float, required=True)
    args = parser.parse_args()

    events = missed = busiest_second = this_second_count = 0
    last_id = None
    this_second = None
    deadline = time.monotonic() + args.seconds
    try:
        sock = connect(args.symbol)
        sock.settimeout(30)
        bid = ask = bid_qty = ask_qty = None
        for message in messages(sock):
            wrapper = json.loads(message)
            data = wrapper["data"]
            if wrapper["stream"].endswith("@bookTicker"):
                bid, ask = scaled(data["b"]), scaled(data["a"])
                bid_qty, ask_qty = scaled(data["B"]), scaled(data["A"])
                line = (0, 0, 0, bid, ask, bid_qty, ask_qty)
            else:
                if last_id is not None and data["t"] != last_id + 1:
                    missed += data["t"] - last_id - 1
                last_id = data["t"]
                if bid is None:
                    continue  # Ravi needs a book before a trade means anything
                side = -1 if data["m"] else 1  # m means the buyer was waiting, so a seller started it
                line = (scaled(data["p"]), scaled(data["q"]), side, bid, ask, bid_qty, ask_qty)
            print(*line, flush=True)

            events += 1
            now = int(time.monotonic())
            this_second_count = this_second_count + 1 if now == this_second else 1
            this_second = now
            busiest_second = max(busiest_second, this_second_count)
            if time.monotonic() >= deadline:
                break
    except (FeedError, OSError, ValueError, KeyError) as error:
        print(f"feed failed after {events} events: {error}", file=sys.stderr)
        return 1
    print("end", flush=True)  # only a healthy feed says this, so a dead feed can't look finished
    print(f"feed: {events} events, {missed} missed, busiest second {busiest_second}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
