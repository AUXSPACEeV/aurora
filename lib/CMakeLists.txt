# Copyright (c) 2025-2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

# filter lib
add_subdirectory_ifdef(CONFIG_FILTER filter)

# sensor lib
add_subdirectory_ifdef(CONFIG_AURORA_SENSORS sensor)

# data lib
add_subdirectory_ifdef(CONFIG_DATA_LOGGER data)

# notification lib
add_subdirectory_ifdef(CONFIG_AURORA_NOTIFY notify)

# powerfail lib
add_subdirectory_ifdef(CONFIG_AURORA_POWERFAIL powerfail)

# pwm melodies
add_subdirectory_ifdef(CONFIG_AURORA_PWM_MELODY pwm_melody)

# state machine lib
add_subdirectory_ifdef(CONFIG_AURORA_STATE_MACHINE state)
