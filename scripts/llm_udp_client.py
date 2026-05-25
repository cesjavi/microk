#!/usr/bin/env python3
import argparse
import socket
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description="MicroK UDP LLM test client")
    parser.add_argument("command", nargs="+", help="PING, STATUS, INFO or ASK <prompt>")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1234)
    parser.add_argument("--timeout", type=float, default=2.0)
    args = parser.parse_args()

    message = " ".join(args.command)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(args.timeout)
        sock.sendto(message.encode("utf-8"), (args.host, args.port))
        try:
            data, _ = sock.recvfrom(2048)
        except socket.timeout:
            print("timeout waiting for UDP response", file=sys.stderr)
            return 1

    print(data.decode("utf-8", errors="replace"), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
