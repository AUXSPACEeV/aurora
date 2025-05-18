#!/bin/env bash
#
# Auxspace avionics application build script
#
# Author: Maximilian Stephan @ Auxspace e.V.
# Copyright (c) 2025 Auxspace e.V.
#

set -e

################################################################################
# Functions                                                                    #
################################################################################

function print_help() {
    echo "Usage:"
    echo "  $0 [OPTIONS] { build | clean | container | docs | setup | shell }"
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
    echo "  docs      ARGS                 Call the docs script with ARGS."
    echo "  menuconfig                     Open the menuconfig for BOARD."
    echo "  setup                          Setup submodules, docker, etc."
    echo "  shell                          Open a shell in the container."
    echo
    echo "Options":
    echo "-b|--board BOARD                 Choose a board config from"
    echo "                                 \"configs/\"."
    echo "                                 Defaults to ${BOARD}."
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

function check_and_build_submodules() {
    set -e

    # setup aurora submodules
    cd "${THISDIR}"
    git submodule update --init

    # setup pico sdk submodules
    cd "${THISDIR}/${_PICO_SDK_REL_PATH}"
    git submodule update --init
}

function run_setup() {
    check_and_build_submodules
    check_and_build_container
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

# Container
BUILDER_APPLICATION=${BUILDER_APPLICATION:-"${THISDIR}"}
BUILDER_APP_SRC=${BUILDER_APP_SRC:-"${BUILDER_APPLICATION}/src"}
FREERTOS_KERNEL_PATH=${FREERTOS_KERNEL_PATH:-"${BUILDER_APP_SRC}/kernel"}

# SDK
_PICO_SDK_REL_PATH=${_PICO_SDK_REL_PATH:-"src/sdk"}
PICO_SDK_PATH=${PICO_SDK_PATH:-"${BUILDER_APPLICATION}/$_PICO_SDK_REL_PATH"}
BOARD=${BOARD:-"pico"}

################################################################################
# Commandline arg parser                                                       #
################################################################################

while [ $# -gt 0 ]; do
    case $1 in
        --container-dir)
            CONTAINER_DIR="$2"
            shift 2
            ;;
        --container-tag)
            CONTAINER_TAG="$2"
            shift 2
            ;;
        -e|--engine)
            CONTAINER_ENGINE="$2"
            shift 2
            ;;
        -b|--board)
            BOARD="$2"
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
            REBUILD_CONTAINER_IMAGE=1
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
            case $1 in
                build|shell|clean|menuconfig)
                    COMMAND="${BOARD:+ --board $BOARD} $1"
                    shift
                    ;;
                setup)
                    if [ -z "$AURORA_CI_BUILD" ]; then
                        COMMAND="/bin/bash"
                    fi
                    run_setup
                    exit 0
                    ;;
                container)
                    do_container "$2"
                    exit 0
                    ;;
                docs)
                    COMMAND="$@"
                    break
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
    -e BUILDER_APPLICATION="${BUILDER_APPLICATION}" \
    -e FREERTOS_KERNEL_PATH="${FREERTOS_KERNEL_PATH}" \
    -e PICO_SDK_PATH="${PICO_SDK_PATH}" \
    -e AURORA_CI_BUILD="$AURORA_CI_BUILD" \
    -v "${THISDIR}:${BUILDER_APPLICATION}:rw" \
    --workdir ${BUILDER_APPLICATION} \
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

# build, start and run container with given command
if [ "$AURORA_CI_BUILD" = "1" ]; then
    run_setup
fi

check_and_build_container
run_container
