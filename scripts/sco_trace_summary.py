#!/usr/bin/env python3
"""Summarize `btmon -r` output from stdin without retaining audio payloads."""

import collections
import json
import re
import statistics
import sys


PACKET = re.compile(
    r"SCO Data (RX|TX): Handle (\d+) flags 0x([0-9a-fA-F]+) dlen (\d+)"
    r".*\[hci\d+\]\s+([0-9]+(?:\.[0-9]+)?)\s*$"
)
STATUSES = ("valid", "possibly_invalid", "no_data", "partially_lost")


def summarize(lines):
    packets = {direction: [] for direction in ("rx", "tx")}
    for line in lines:
        match = PACKET.search(line)
        if match:
            direction, handle, flags, length, timestamp = match.groups()
            packets[direction.lower()].append(
                (int(handle), int(flags, 16), int(length), float(timestamp))
            )

    result = {}
    for direction, entries in packets.items():
        sizes = collections.Counter(entry[2] for entry in entries)
        statuses = collections.Counter(entry[1] for entry in entries)
        previous = {}
        gaps = []
        for handle, _, _, timestamp in entries:
            if handle in previous and timestamp >= previous[handle]:
                gaps.append((timestamp - previous[handle]) * 1000)
            previous[handle] = timestamp
        result[direction] = {
            "count": len(entries),
            "lengths": dict(sorted(sizes.items())),
            "status": {**{name: statuses[index] for index, name in enumerate(STATUSES)},
                       "other": sum(count for flags, count in statuses.items() if flags > 3)},
            "gap_ms": {"median": statistics.median(gaps), "max": max(gaps)} if gaps else None,
        }
    return result


if __name__ == "__main__":
    json.dump(summarize(sys.stdin), sys.stdout, indent=2)
    print()
