#!/usr/bin/env python3
"""Client for the port's debug console (src/debug_server.cpp).

    python tools/wdbg.py help               one command, print the reply
    python tools/wdbg.py "shot a.png" stats several commands in order
    python tools/wdbg.py                    interactive prompt

WETRIX_DEBUG_PORT picks the port (default 7464).
"""
import os
import socket
import sys


def connect():
    port = int(os.environ.get("WETRIX_DEBUG_PORT", "7464"))
    s = socket.create_connection(("127.0.0.1", port), timeout=30)
    return s, s.makefile("r", encoding="utf-8", errors="replace")


def ask(sock, reader, line):
    sock.sendall((line.strip() + "\n").encode())
    out = []
    for l in reader:
        if l.rstrip("\n") == ".":
            break
        out.append(l)
    return "".join(out)


def main():
    try:
        sock, reader = connect()
    except OSError as e:
        print(f"cannot reach the game's debug console: {e}", file=sys.stderr)
        return 1
    if len(sys.argv) > 1:
        for cmd in sys.argv[1:]:
            sys.stdout.write(ask(sock, reader, cmd))
        return 0
    while True:
        try:
            line = input("wetrix> ")
        except EOFError:
            return 0
        if line.strip():
            sys.stdout.write(ask(sock, reader, line))


if __name__ == "__main__":
    sys.exit(main())
