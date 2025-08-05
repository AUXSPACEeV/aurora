#!/bin/env bash
#
# Auxspace bash logging lib
#
# Just a simple lib for logging
#
# Author: Maximilian Stephan @ Auxspace e.V.
# Copyright (c) 2025 Auxspace e.V.
#

declare -x -i LOG_LEVEL_ERR=0
declare -x -i LOG_LEVEL_WARN=1
declare -x -i LOG_LEVEL_INFO=2
declare -x -i LOG_LEVEL_DEBUG=3
declare -x -i LOG_LEVEL=${LOG_LEVEL_INFO}

function log_debug() {
    if [ $LOG_LEVEL -ge $LOG_LEVEL_DEBUG ]; then
        echo "DEBUG: $@"
    fi
}

function log_info() {
    if [ $LOG_LEVEL -ge $LOG_LEVEL_INFO ]; then
        echo "INFO: $@"
    fi
}

function log_warn() {
    if [ $LOG_LEVEL -ge $LOG_LEVEL_WARN ]; then
        >&2 echo "WARNING: $@"
    fi
}

function log_err() {
    if [ $LOG_LEVEL -ge $LOG_LEVEL_ERR ]; then
        >&2 echo "ERROR: $@"
    fi
}