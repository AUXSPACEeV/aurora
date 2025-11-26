#!/bin/env bash
#
# Auxspace container management functions
#
# note: make sure logging is included before sourcing this script
#
# Author: Maximilian Stephan @ Auxspace e.V.
# Copyright (c) 2025 Auxspace e.V.
#

if [ -z "$THISDIR" ]; then
    declare -x THISDIR="$(pwd)"
    log_warn "THISDIR has not been defined yet. Defaulting to \"$THISDIR\"."
fi

if [ -z "$DISTRO_ARCH" ]; then
    case "$(uname -m)" in
        x86_64)
            DISTRO_ARCH="x86_64"
            ;;
        aarch64 | arm64)
            DISTRO_ARCH="arm64"
            ;;
        *)
            echo "Unsupported architecture: $(uname -m)" >&2
            return 1
            ;;
    esac
fi

# local r/o
declare -r _CONTAINER_NAME="auxspace-avionics-builder"

if [ -z "$AURORA_CI_BUILD" ]; then
    declare -x AURORA_CI_BUILD=0
fi

# global r/w for container config
declare -x CONTAINER_TAG_FILE="${THISDIR}/container/version"
declare -x CONTAINER_TAG="$(cat ${CONTAINER_TAG_FILE})"
declare -x CONTAINER_DIR="${THISDIR}/container"
declare -x CONTAINER_ENGINE="docker"
declare -x CONTAINER_BIN="docker"
declare -x CONTAINER_BUILD_BIN="${CONTAINER_BIN} buildx"
declare -x -i REBUILD_CONTAINER_IMAGE=0
declare -x CONTAINER_PULL_NAME="ghcr.io/auxspaceev/${_CONTAINER_NAME}:${CONTAINER_TAG}"

# Default to interactive flags unless running in CI
if [ -z "$AURORA_CI_BUILD" ] || ! [[ "$AURORA_CI_BUILD" =~ ^[1-9][0-9]*$ ]]; then
    # Not in CI or invalid/non-numeric → enable interactive flags
    declare -x CONTAINER_EXEC_FLAGS="-it"
elif [ "$AURORA_CI_BUILD" -eq 0 ]; then
    declare -x CONTAINER_EXEC_FLAGS="-it"
else
    # CI is enabled and properly set
    declare -x CONTAINER_EXEC_FLAGS=""
fi

# this should only be appended and not overwritten
CONTAINER_RUNTIME_ARGS=" \
    ${CONTAINER_EXEC_FLAGS} \
    --name ${_CONTAINER_NAME} \
"

function set_container_bins() {
    if [ "${CONTAINER_ENGINE}" = "podman" ]; then
        CONTAINER_BIN="podman"
        CONTAINER_BUILD_BIN="buildah"
    elif [ "${CONTAINER_ENGINE}" = "docker" ]; then
        CONTAINER_BIN="docker"
        CONTAINER_BUILD_BIN="docker buildx"
    else
        log_err "Unknown container engine: $CONTAINER_ENGINE"
        exit 1
    fi
}

function pull_container() {
    log_info "Pulling '${CONTAINER_PULL_NAME}' ..."
    if [ -z "${CONTAINER_BIN}" ]; then
        log_err "No conainer bin passed."
    fi

    ${CONTAINER_BIN} pull ${CONTAINER_PULL_NAME}
}

function check_and_build_container() {
    set_container_bins

    log_info "Checking for existing $CONTAINER_BIN images ..."

    # Check if a container with the specified name exists
    if [ $REBUILD_CONTAINER_IMAGE -eq 1 ]; then
        log_info "Force rebuild activated."
        build_container
    elif ! ${CONTAINER_BIN} images -a --format '{{.Repository}}' \
        | grep -q "$_CONTAINER_NAME"; then
        # Try to pull container from ghcr.io
        log_info "Searching for container '$CONTAINER_PULL_NAME' ..."
        
        pull_container && return

        # Not found: build it locally
        log_warn "Image for container '$CONTAINER_PULL_NAME' not found. Building ..."
        build_container
    else
        log_info "Image for container '${_CONTAINER_NAME}:$CONTAINER_TAG' already exists."
        log_info "Skipping container build."
    fi
}

function build_container() {
    log_info "Building container '${_CONTAINER_NAME}:$CONTAINER_TAG' ..."
    if [ -z "${CONTAINER_BUILD_BIN}" ]; then
        log_err "No container build command passed."
        exit 1
    fi

    DOCKER_BUILDKIT=1 ${CONTAINER_BUILD_BIN} build \
        --build-arg PUID=$(id -u) \
        --build-arg PGID=$(id -g) \
        --tag "${CONTAINER_PULL_NAME}" \
        --file "$CONTAINER_DIR/Dockerfile_$DISTRO_ARCH" \
        "$CONTAINER_DIR"
}

function stop_container() {
    # Stop the container if its running
    if [ -n "$($CONTAINER_BIN ps -a | grep ${_CONTAINER_NAME})" ]; then
        log_info "Stopping running container ..."
        $CONTAINER_BIN stop ${_CONTAINER_NAME}
        log_info "Container has been stopped."
    else
        log_debug "Container \"${_CONTAINER_NAME}\" is not running."
    fi
}

function remove_container() {
    # Stop a container first
    stop_container

    if [ -n "$($CONTAINER_BIN container ls -a \
        | grep ${_CONTAINER_NAME})" ]; then
        log_info "Removing container ..."
        $CONTAINER_BIN container rm ${_CONTAINER_NAME}
        log_info "Container has been removed."
    else
        log_debug "Container \"${_CONTAINER_NAME}\" does not exist."
    fi
}

function remove_container_image() {
    # stop and remove the container before removing the image
    remove_container

    # remove image
    if [ -n "$($CONTAINER_BIN images -a | grep ${_CONTAINER_NAME})" ]; then
        log_info "Removing container image ..."
        $CONTAINER_BIN image rm "${CONTAINER_PULL_NAME}"
        log_info "Container image has been removed."
    else
        log_debug "No container image to remove."
    fi
}

function start_container() {
    log_info "Starting container $_CONTAINER_NAME"
    $CONTAINER_BIN start $_CONTAINER_NAME
}

function run_container_cmd() {
    local use_run_cmd="${1:-0}"
    local run_cmd="shell"

    if [ "$AURORA_CI_BUILD" = "1" ]; then
        run_cmd="$COMMAND"
    fi

    if [ "$use_run_cmd" = "1" ]; then
        log_info "Running '${_CONTAINER_NAME}:$CONTAINER_TAG' ..."
        exec $CONTAINER_BIN run \
            $run_cmd_args \
            $CONTAINER_RUNTIME_ARGS \
            ${CONTAINER_PULL_NAME} \
            ${run_cmd}
    fi

    if ! ${CONTAINER_BIN} ps --format '{{.Names}}' \
        | grep -q "$_CONTAINER_NAME"; then
        log_warn "Container '$_CONTAINER_NAME' is stopped. Using 'start'."
        start_container
    fi

    log_info "Attaching to running container '$_CONTAINER_NAME' ..."
    $CONTAINER_BIN exec \
        $CONTAINER_EXEC_FLAGS \
        $_CONTAINER_NAME \
        /sbin/entrypoint $COMMAND
}

function run_container() {
    local use_run_cmd="0"
    if ! ${CONTAINER_BIN} container ls -a --format '{{.Names}}' \
        | grep -q "$_CONTAINER_NAME"; then
        log_warn "Container '$_CONTAINER_NAME' does not exist. Using 'run'."
        use_run_cmd="1"
    elif ! ${CONTAINER_BIN} ps --format '{{.Names}}' \
        | grep -q "$_CONTAINER_NAME"; then
        log_warn "Container '$_CONTAINER_NAME' is stopped. Using 'start'."
        start_container
    fi

    run_container_cmd $use_run_cmd
}

function do_container() {
    local do_container_cmd="${1}"

    if [ -z "${do_container_cmd}" ]; then
        log_err "No container command has been given."
        exit 1
    fi

    set_container_bins

    case $do_container_cmd in
        build)
            check_and_build_container
            ;;
        rm)
            remove_container_image
            ;;
        run)
            run_container
            ;;
        start)
            start_container
            ;;
        stop)
            stop_container
            ;;
        *)
            log_err "Container command \"$do_container_cmd\" is invalid."
            exit 1
            ;;
    esac
}
