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

function check_and_build_container() {
    log_info "Checking container engine ..."
    if [ "${CONTAINER_ENGINE}" = "podman" ]; then
        _CONTAINER_BIN="podman"
        _CONTAINER_BUILD_BIN="buildah"
    elif [ "${CONTAINER_ENGINE}" = "docker" ]; then
        _CONTAINER_BIN="docker"
        _CONTAINER_BUILD_BIN="docker buildx"
    else
        log_err "Unknown container engine: $CONTAINER_ENGINE"
        exit 1
    fi

    # Check if a container with the specified name exists
    log_info "Checking for existing $_CONTAINER_BIN images ..."
    if ! ${_CONTAINER_BIN} images -a --format '{{.Repository}}' \
        | grep -q "$CONTAINER_NAME"; then
        log_warn "Image for container '$CONTAINER_NAME' not found."
        build_container
    elif [ -n "$REBUILD_CONTAINER_IMAGE" ]; then
        log_info "Force rebuild activated."
        build_container
    else
        log_info "Image for container '$CONTAINER_TAG' already exists."
        log_info "Skipping container build."
    fi
}

function build_container() {
    log_info "Building container '$CONTAINER_TAG' ..."
    if [ -z "${_CONTAINER_BUILD_BIN}" ]; then
        log_err "No container build command passed."
        exit 1
    fi

    DOCKER_BUILDKIT=1 ${_CONTAINER_BUILD_BIN} build \
        --build-arg PUID=$(id -u) \
        --build-arg PGID=$(id -g) \
        --tag $CONTAINER_TAG \
        --file "$CONTAINER_DIR/Dockerfile" \
        "$CONTAINER_DIR"
}

function remove_container() {
    log_info "Removing aurora build container '$CONTAINER_NAME'."

    # Stop running container
    if [ -n "$($_CONTAINER_BIN ps -a | grep ${CONTAINER_NAME})" ]; then
        log_info "Stopping running container ..."
        $_CONTAINER_BIN stop ${CONTAINER_NAME}
    fi

    # remove container
    if [ -n "$($_CONTAINER_BIN container ls -a \
        | grep ${CONTAINER_NAME})" ]; then
        log_info "Removing container ..."
        $_CONTAINER_BIN container rm ${CONTAINER_NAME}
    fi

    # remove image
    if [ -n "$($_CONTAINER_BIN images -a | grep ${CONTAINER_NAME})" ]; then
        log_info "Removing container image ..."
        $_CONTAINER_BIN image rm ${CONTAINER_TAG}
    fi

    log_info "Container has been removed."
}

function start_container() {
    log_info "Starting container $CONTAINER_NAME"
    $_CONTAINER_BIN start $CONTAINER_NAME
}

function run_container_cmd() {
    local use_run_cmd="${1:-0}"
    if [ "$use_run_cmd" = "1" ]; then
        log_info "Running '$CONTAINER_TAG' ..."
        $_CONTAINER_BIN run \
            $CONTAINER_RUNTIME_ARGS \
            $CONTAINER_TAG \
            $COMMAND
    else
        log_info "Attaching to running container '$CONTAINER_NAME' ..."
        $_CONTAINER_BIN exec -it \
            $CONTAINER_NAME \
            /sbin/entrypoint $COMMAND
    fi
}

function run_container() {
    local use_run_cmd="0"
    if ! ${_CONTAINER_BIN} container ls -a --format '{{.Names}}' \
        | grep -q "$CONTAINER_NAME"; then
        log_warn "Container '$CONTAINER_NAME' does not exist. Using 'run'."
        use_run_cmd="1"
    elif ! ${_CONTAINER_BIN} ps --format '{{.Names}}' \
        | grep -q "$CONTAINER_NAME"; then
        log_warn "Container '$CONTAINER_NAME' is stopped. Using 'start'."
        start_container
    fi

    run_container_cmd $use_run_cmd
}

function run_setup() {
    check_and_build_submodules
    check_and_build_container
}

function do_container() {
    local do_container_cmd="${1:-build}"
    if [ "$do_container_cmd" = "build" ]; then
        check_and_build_container
    elif [ "$do_container_cmd" = "rm" ]; then
        remove_container
    else
        log_err "Container command \"$do_container_cmd\" is invalid."
        return 1
    fi
}

################################################################################
# Variables and constants                                                      #
################################################################################

# THISDIR
unameOut="$(uname -s)"
case "${unameOut}" in
    Linux*)     machine=Linux;;
    Darwin*)    machine=Mac;;
    *)          machine="${unameOut}"
esac

log_info "Detected System: $machine"

if [ "$machine" = "Mac" ]; then
    THISDIR="$(dirname $(readlink -f $0))"
else
    THISDIR="$(dirname $(readlink -e -- $0))"
fi

# Logging
declare -i LOG_LEVEL_ERR=0
declare -i LOG_LEVEL_WARN=1
declare -i LOG_LEVEL_INFO=2
declare -i LOG_LEVEL_DEBUG=3
declare -i LOG_LEVEL=${LOG_LEVEL_INFO}

# Container
CONTAINER_DIR="${THISDIR}/container"
CONTAINER_NAME="auxspace-avionics-builder"
CONTAINER_TAG="${CONTAINER_NAME}:local"
CONTAINER_ENGINE="docker"
_CONTAINER_BIN="docker"
_CONTAINER_BUILD_BIN="${_CONTAINER_BIN} buildx"
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

CONTAINER_RUNTIME_ARGS=" \
    -it \
    --name ${CONTAINER_NAME} \
    -e PUID=`id -u` \
    -e PGID=`id -g` \
    -e BUILDER_APPLICATION="${BUILDER_APPLICATION}" \
    -e FREERTOS_KERNEL_PATH="${FREERTOS_KERNEL_PATH}" \
    --user $(id -u):$(id -g) \
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
