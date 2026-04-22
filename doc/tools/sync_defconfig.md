# sync_defconfig.py

Merge the Zephyr-generated `defconfig` into a board-specific `.conf`.

## What it does

After a Zephyr build, `build/zephyr/kconfig/defconfig` holds the fully
resolved Kconfig state for the chosen board. `sync_defconfig.py` folds that
snapshot back into the tracked `boards/<board>.conf` without losing any
manual overrides and without re-stating anything that is already covered
by the project-wide `prj.conf`.

The merge walks the generated `defconfig` line by line and produces a
merged `<project>/boards/<board>.conf` that:

- follows `defconfig`'s ordering and structure;
- drops any entry already covered by `<project>/prj.conf` (overhead
  cleanup);
- keeps the existing `board.conf` value for keys that are already set
  there (manual overrides win);
- fills in everything else from `defconfig`;
- appends any `board.conf`-only entries (not in `defconfig` and not in
  `prj.conf`) at the end.

Blank lines and comments from `defconfig` are preserved so the output
keeps the same visual grouping.

## Usage

```
python3 tools/sync_defconfig.py <board> <project>
```

Arguments:

- `board` — Zephyr board name (e.g. `sensor_board_v2/rp2040`). Slashes are
  translated to underscores when resolving the `.conf` path.
- `project` — AURORA project directory (e.g. `sensor_board`).

Expected paths (relative to the `aurora/` root):

- `build/zephyr/kconfig/defconfig` — must exist from a prior `west build`.
- `<project>/prj.conf` — must exist.
- `<project>/boards/<board>.conf` — written (created if missing).

Example:

```bash
west build -b sensor_board_v2/rp2040 aurora/sensor_board
python3 aurora/tools/sync_defconfig.py sensor_board_v2/rp2040 sensor_board
```

The script reports a summary on stdout:

```
Written aurora/sensor_board/boards/sensor_board_v2_rp2040.conf
  42 kept, 7 added from defconfig, 3 removed (overhead in prj.conf)
```

## Requirements

- Python 3.10+
- A completed Zephyr build for the target board.
