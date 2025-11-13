#!/bin/env bash
#
# Zephyr application build script
#
# Author: Maximilian Stephan @ Auxspace e.V.
# Copyright (c) 2024 Auxspace e.V.
#

set -e
set -x

################################################################################
# Functions                                                                    #
################################################################################

function print_help() {
    echo "Usage:"
    echo "  $0 [OPTIONS] { build | clean | container| menuconfig | shell }"
    echo
    echo "Positional arguments:"
    echo "  build                          Build the platform image."
    echo "  clean                          Clean the build directory."
    echo "  container CMD                  Manage the dev container."
    echo "                                 Commands are:"
    echo "                                   - build"
    echo "                                   - rm"
    echo "                                   - run"
    echo "                                   - start"
    echo "                                   - stop"
    echo "  menuconfig                     Open the menuconfig for BOARD."
    echo "  shell                          Open a shell in the container."
    echo
    echo "Options":
    echo "-b|--board BOARD                 Choose a board config for zephyr."
    echo "                                 Defaults to ${ZEPHYR_BOARD}."
    echo "   --container-dir DIR           Use custom container directory."
    echo "                                 Defaults to ${CONTAINER_DIR}."
    echo "   --container-tag TAG           Tag of the container."
    echo "                                 Defaults to ${CONTAINER_TAG}."
    echo "-e|--engine { docker | podman }  Use a specific container engine."
    echo "                                 Defaults to ${CONTAINER_ENGINE}."
    echo "-h|--help                        Print this help text."
    echo "-l|--log-level LEVEL             Set log level to LEVEL."
    echo "                                 -1 -> logging OFF"
    echo "                                 $LOG_LEVEL_ERR  -> ERROR"
    echo "                                 $LOG_LEVEL_WARN  -> WARN"
    echo "                                 $LOG_LEVEL_INFO  -> INFO"
    echo "                                 $LOG_LEVEL_DEBUG  -> DEBUG"
    echo "   --rebuild                     Rebuild the container image even"
    echo "                                 if it exists."
    echo "-v|--verbose                     Set DEBUG log level (+set -x)."
}

################################################################################
# Basic setup                                                                  #
################################################################################

# THISDIR
unameOut="$(uname -s)"
case "${unameOut}" in
    Linux*)     machine=Linux;;
    Darwin*)    machine=Mac;;
    *)          machine="${unameOut}"
esac

# No logging yet, need THISDIR first
echo "Detected System: $machine"

if [ "$machine" = "Mac" ]; then
    declare -x -r THISDIR="$(dirname $(readlink -f $0))"
else
    declare -x -r THISDIR="$(dirname $(readlink -e -- $0))"
fi

################################################################################
# Includes                                                                     #
################################################################################

source $THISDIR/scripts/lib/log.sh
source $THISDIR/scripts/lib/container.sh

################################################################################
# Variables and constants                                                      #
################################################################################

# Environment from file
if [ -f "$THISDIR/aurora.env" ]; then
    source "$THISDIR/aurora.env"
fi

# Zephyr config
AURORA_APPLICATION_DIR=${AURORA_APPLICATION_DIR:-"${THISDIR}"}
ZEPHYR_SOURCE_SW_VERSION=${ZEPHYR_SOURCE_SW_VERSION:-"main"}
ZEPHYR_APPLICATION=${ZEPHYR_APPLICATION:-"sensor_board"}
ZEPHYR_WORKSPACE=${ZEPHYR_WORKSPACE:-"$(dirname ${THISDIR})"}
ZEPHYR_BOARD=${ZEPHYR_BOARD:-"rpi_pico"}

################################################################################
# Commandline arg parser                                                       #
################################################################################

while [ $# -gt 0 ]; do
    case $1 in
        -b|--board)
            ZEPHYR_BOARD="$2"
            shift 2
            ;;
        --container-dir)
            CONTAINER_DIR="$2"
            shift 2
            ;;
        -e|--engine)
            CONTAINER_ENGINE="$2"
            shift 2
            ;;
        -h|--help)
            shift
            print_help
            exit 0
            ;;
        -l|--log-level)
            LOG_LEVEL=$2
            shift 2
            ;;
        --rebuild)
            REBUILD_CONTAINER_IMAGE="1"
            shift
            ;;
        -v|--verbose)
            set -x
            LOG_LEVEL=$LOG_LEVEL_DEBUG
            shift 1
            ;;
        --*)
            log_err "No such option: $1"
            print_help
            exit 1
            ;;
        *)
            COMMAND="$1"
            case "${COMMAND}" in
                build|shell|clean|menuconfig)
                    COMMAND="${BOARD:+ --board $ZEPHYR_BOARD} $1"
                    shift
                    ;;
                container)
                    do_container "$2"
                    exit 0
                    ;;
                *)
                    log_err "No such command: ${COMMAND}"
                    print_help
                    exit 1
                    ;;
                esac
            ;;
    esac
done

################################################################################
# Build vars from argparsing                                                   #
################################################################################

CONTAINER_RUNTIME_ARGS+=" \
    -e ZEPHYR_APPLICATION="${ZEPHYR_APPLICATION}" \
    -e AURORA_APPLICATION_DIR="${AURORA_APPLICATION_DIR}" \
    -e ZEPHYR_WORKSPACE="${ZEPHYR_WORKSPACE}" \
    -e ZEPHYR_BOARD="${ZEPHYR_BOARD}" \
    -v "${ZEPHYR_WORKSPACE}:${ZEPHYR_WORKSPACE}:rw" \
    --workdir ${AURORA_APPLICATION_DIR} \
"

if [ "$CONTAINER_ENGINE" = "docker" ]; then
    CONTAINER_RUNTIME_ARGS+=" \
        -e PUID=`id -u` \
        -e PGID=`id -g` \
        --user $(id -u):$(id -g) \
    "
fi

################################################################################
# Error checking                                                               #
################################################################################

if [ -z "${COMMAND}" ]; then
    log_err "No command specified."
    print_help
    exit 1
fi

################################################################################
# Main                                                                         #
################################################################################

# run entrypoint directly if in container
if [ -x /sbin/entrypoint ]; then
    exec /sbin/entrypoint $COMMAND
fi

################################################################################
# Main                                                                         #
################################################################################

check_and_build_container

run_container
