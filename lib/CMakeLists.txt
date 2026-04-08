# Copyright (c) 2025-2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

# apogee detection lib
add_subdirectory_ifdef(CONFIG_APOGEE_DETECTION apogee)

# sensor lib
add_subdirectory_ifdef(CONFIG_AURORA_SENSORS sensor)

# data lib
add_subdirectory_ifdef(CONFIG_DATA_LOGGER data)

# state machine lib
add_subdirectory_ifdef(CONFIG_AURORA_STATE_MACHINE state)

# notification lib
add_subdirectory_ifdef(CONFIG_AURORA_NOTIFY notify)
