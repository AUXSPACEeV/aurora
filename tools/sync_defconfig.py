#!/usr/bin/env python3
#
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Merge defconfig into a board-specific config file.

Walks build/zephyr/kconfig/defconfig line by line and produces a merged
<project>/boards/<board>.conf that:
  - follows defconfig's ordering and structure
  - removes any entry already covered by <project>/prj.conf (overhead cleanup)
  - uses the existing board.conf value for keys that are already there
  - fills in everything else from defconfig
  - appends any board.conf-only entries (not in defconfig) at the end
"""

import argparse
import re
import sys
from pathlib import Path

AURORA_ROOT = Path(__file__).parent.parent

KEY_RE = re.compile(r'^(CONFIG_\w+)=|^#\s*(CONFIG_\w+)\s+is not set')


def extract_key(line: str) -> str | None:
    m = KEY_RE.match(line)
    return (m.group(1) or m.group(2)) if m else None


def parse_key_lines(path: Path) -> dict[str, str]:
    """Return {key: line} for all config entries in a file."""
    result = {}
    if not path.exists():
        return result
    for line in path.read_text().splitlines():
        key = extract_key(line)
        if key:
            result[key] = line
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('board', help='Board name (e.g. sensor_board_v2/rp2040)')
    parser.add_argument('project', help='Project directory name (e.g. sensor_board)')
    args = parser.parse_args()

    board = args.board.replace("/", "_")

    defconfig_path = AURORA_ROOT / 'build' / 'zephyr' / 'kconfig' / 'defconfig'
    prj_conf_path  = AURORA_ROOT / args.project / 'prj.conf'
    board_conf_path = AURORA_ROOT / args.project / 'boards' / f'{board}.conf'

    if not defconfig_path.exists():
        sys.exit(f'Error: defconfig not found at {defconfig_path}')
    if not prj_conf_path.exists():
        sys.exit(f'Error: prj.conf not found at {prj_conf_path}')

    prj_keys   = set(parse_key_lines(prj_conf_path))
    board_lines = parse_key_lines(board_conf_path)  # key -> existing line

    # Walk defconfig and build the merged output
    output: list[str] = []
    seen_keys: set[str] = set()

    for line in defconfig_path.read_text().splitlines():
        key = extract_key(line)
        if key is None:
            # Blank line or comment — keep for structure
            output.append(line)
            continue
        if key in prj_keys:
            # Covered by prj.conf — drop entirely
            continue
        seen_keys.add(key)
        # Use existing board.conf line if present, otherwise take from defconfig
        output.append(board_lines.get(key, line))

    # Append board.conf-only entries (keys not present in defconfig and not in prj.conf)
    board_only = [line for key, line in board_lines.items()
                  if key not in seen_keys and key not in prj_keys]
    if board_only:
        output.append('')
        output.extend(board_only)

    # Strip leading/trailing blank lines, ensure single trailing newline
    while output and output[0] == '':
        output.pop(0)
    while output and output[-1] == '':
        output.pop()

    board_conf_path.parent.mkdir(parents=True, exist_ok=True)
    board_conf_path.write_text('\n'.join(output) + '\n')

    added   = len([k for k in seen_keys if k not in board_lines])
    kept    = len([k for k in seen_keys if k in board_lines])
    removed = len([k for k in board_lines if k in prj_keys])
    print(f'Written {board_conf_path}')
    print(f'  {kept} kept, {added} added from defconfig, {removed} removed (overhead in prj.conf)')


if __name__ == '__main__':
    main()
