# Copyright (c) 2025 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

# console lib
add_subdirectory_ifdef(CONFIG_AURORA_CONSOLE console)

# sensor lib
add_subdirectory_ifdef(CONFIG_AURORA_SENSORS sensor)

# state machine lib
add_subdirectory_ifdef(CONFIG_AURORA_STATE_MACHINE state)
