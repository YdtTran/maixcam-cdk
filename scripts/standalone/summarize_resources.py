#!/usr/bin/env python3
"""Summarize two /proc samples without assuming a kernel clock-tick frequency."""
import argparse
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('report', type=Path)
args = parser.parse_args()
text = args.report.read_text()
cpu = [sum(map(int, line.split()[1:9])) for line in text.splitlines() if line.startswith('cpu ')]
processes = {}
for line in text.splitlines():
    match = re.match(r'^(\d+) \(([^)]+)\) (.+)$', line)
    if match:
        fields = match[3].split()
        processes.setdefault((match[1], match[2]), []).append(int(fields[11]) + int(fields[12]))
result = {'source': str(args.report), 'pss': 'unavailable unless present in raw report', 'cpu_percent': {}}
if len(cpu) >= 2 and cpu[-1] > cpu[0]:
    for (pid, name), ticks in processes.items():
        if len(ticks) >= 2:
            result['cpu_percent'][f'{name} ({pid})'] = round(100 * (ticks[-1] - ticks[0]) / (cpu[-1] - cpu[0]), 2)
print(json.dumps(result, indent=2))
