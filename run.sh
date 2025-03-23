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
    echo "  $0 [OPTIONS] { build | checkout | container | clean | shell }"
    echo
    echo "Positional arguments:"
    echo "  build                          Build the platform image."
    echo "  clean                          Clean the build directory."
    echo "  container { build | rm }       Build or remove the dev container."
    echo "  setup                          Setup submodules, docker, etc."
    echo "  shell                          Open a shell in the container."
    echo
    echo "Options":
    echo "   --container-dir DIR           Use custom container directory."
    echo "                                 Defaults to ${CONTAINER_DIR}."
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
    cd $THISDIR && git submodule init && git submodule update
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

# Container
BUILDER_WORKSPACE="/builder/workspace"
BUILDER_APPLICATION="${BUILDER_WORKSPACE}/aurora"
FREERTOS_KERNEL_PATH="${BUILDER_APPLICATION}/src/kernel"

################################################################################
# Commandline arg parser                                                       #
################################################################################

while [ $# -gt 0 ]; do
    case $1 in
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
                build|shell|clean)
                    COMMAND="$1"
                    shift
                    ;;
                setup)
                    run_setup
                    exit 0
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
    -e BUILDER_APPLICATION="${BUILDER_APPLICATION}" \
    -e FREERTOS_KERNEL_PATH="${FREERTOS_KERNEL_PATH}" \
    -v "${THISDIR}:${BUILDER_APPLICATION}:rw" \
    --workdir ${BUILDER_APPLICATION} \
"

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
check_and_build_container
run_container
