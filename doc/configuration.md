# Configuration

All AURORA features are toggled and tuned through Kconfig.  Use
`west build -b <board> -t menuconfig` or `./run.sh -b <board> menuconfig` to
browse options interactively.

By hitting `D`, menuconfig supports minimal config saving.
After saving a minimal config, run `tools/sync_defconfig.py` to merge
the minimal config automatically into the desired project.

````{warning}
sync_defconfig.py is not an official part of zephyr, so use the script at own
risk! (The high risk being that a config might be different from what you were
expecting)
````
