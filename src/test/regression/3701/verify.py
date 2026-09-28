#!/usr/bin/env python3
"""Verifies the uplink AQM regression test output.

With the uplink router in place, a UDP sender should be able to enqueue a burst
of datagrams much faster than the configured upload bandwidth would allow if the
socket send buffer were the rate limiter. The excess is absorbed by the uplink
router's CoDel queue, which drops packets under sustained overload; consequently
the receiver observes fewer bytes than were sent.
"""

import argparse
import glob
import re
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--min-sent", type=int, required=True)
    parser.add_argument("--max-elapsed", type=float, required=True)
    args = parser.parse_args()

    sent = 0
    elapsed = None
    received = 0

    for path in glob.glob("hosts/*/*.stdout"):
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()

        for m in re.finditer(r"sent=(\d+)\s+elapsed=([0-9.]+)", text):
            sent = max(sent, int(m.group(1)))
            elapsed = float(m.group(2))

        for m in re.finditer(r"recv=(\d+)", text):
            received = max(received, int(m.group(1)))

    print(f"sent={sent} received={received} elapsed={elapsed}")

    if sent < args.min_sent:
        print(f"FAIL: sent {sent} < required {args.min_sent}")
        return 1
    if elapsed is None:
        print("FAIL: did not find the sender's elapsed time")
        return 1
    if elapsed > args.max_elapsed:
        print(
            f"FAIL: sender took {elapsed:.3f}s to enqueue {sent} bytes, which is "
            f"more than the allowed {args.max_elapsed}s; EWOULDBLOCK is still "
            f"rate-limiting the sender"
        )
        return 1
    if received >= sent:
        print(
            f"FAIL: receiver got all {received} bytes, so the uplink queue did "
            f"not drop under CoDel"
        )
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
