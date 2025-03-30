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

# local r/o
declare -r _CONTAINER_NAME="auxspace-avionics-builder"

# global r/w for container config
declare -x CONTAINER_TAG="local"
declare -x CONTAINER_DIR="${THISDIR}/container"
declare -x CONTAINER_ENGINE="docker"
declare -x CONTAINER_BIN="docker"
declare -x CONTAINER_BUILD_BIN="${CONTAINER_BIN} buildx"
declare -x -i REBUILD_CONTAINER_IMAGE=0

if [ -z "$AURORA_CI_BUILD" ] || [ "$AURORA_CI_BUILD" -eq 0 ]; then
    declare -x CONTIANER_EXEC_FLAGS="-it"
else
    declare -x CONTIANER_EXEC_FLAGS=""
fi

# this should only be appended and not overwritten
CONTAINER_RUNTIME_ARGS=" \
    ${CONTIANER_EXEC_FLAGS} \
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

function check_and_build_container() {
    set_container_bins

    # Check if a container with the specified name exists
    log_info "Checking for existing $CONTAINER_BIN images ..."
    if ! ${CONTAINER_BIN} images -a --format '{{.Repository}}' \
        | grep -q "$_CONTAINER_NAME"; then
        log_warn "Image for container '$_CONTAINER_NAME' not found."
        build_container
    elif [ $REBUILD_CONTAINER_IMAGE -eq 1 ]; then
        log_info "Force rebuild activated."
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
        --tag "${_CONTAINER_NAME}:$CONTAINER_TAG" \
        --file "$CONTAINER_DIR/Dockerfile" \
        "$CONTAINER_DIR"
}

function remove_container() {
    log_info "Removing aurora build container '$_CONTAINER_NAME'."

    # Stop running container
    if [ -n "$($CONTAINER_BIN ps -a | grep ${_CONTAINER_NAME})" ]; then
        log_info "Stopping running container ..."
        $CONTAINER_BIN stop ${_CONTAINER_NAME}
    fi

    # remove container
    if [ -n "$($CONTAINER_BIN container ls -a \
        | grep ${_CONTAINER_NAME})" ]; then
        log_info "Removing container ..."
        $CONTAINER_BIN container rm ${_CONTAINER_NAME}
    fi

    # remove image
    if [ -n "$($CONTAINER_BIN images -a | grep ${_CONTAINER_NAME})" ]; then
        log_info "Removing container image ..."
        $CONTAINER_BIN image rm "${_CONTAINER_NAME}:${CONTAINER_TAG}"
    fi

    log_info "Container has been removed."
}

function start_container() {
    log_info "Starting container $_CONTAINER_NAME"
    $CONTAINER_BIN start $_CONTAINER_NAME
}

function run_container_cmd() {
    local use_run_cmd="${1:-0}"
    local run_cmd="shell"
    local run_cmd_args="-d"

    if [ "$AURORA_CI_BUILD" = "1" ]; then
        run_cmd="$COMMAND"
        run_cmd_args=""
    fi

    if [ "$use_run_cmd" = "1" ]; then
        log_info "Running '${_CONTAINER_NAME}:$CONTAINER_TAG' ..."
        $CONTAINER_BIN run \
            $run_cmd_args \
            $CONTAINER_RUNTIME_ARGS \
            ${_CONTAINER_NAME}:$CONTAINER_TAG \
            ${run_cmd}
        if [ "$AURORA_CI_BUILDER" = "1" ]; then
            exit 0
        fi
    fi

    if ! ${CONTAINER_BIN} ps --format '{{.Names}}' \
        | grep -q "$_CONTAINER_NAME"; then
        log_warn "Container '$_CONTAINER_NAME' is stopped. Using 'start'."
        start_container
    fi

    log_info "Attaching to running container '$_CONTAINER_NAME' ..."
    $CONTAINER_BIN exec \
        $CONTIANER_EXEC_FLAGS \
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
    local do_container_cmd="${1:-build}"
    set_container_bins

    if [ "$do_container_cmd" = "build" ]; then
        check_and_build_container
    elif [ "$do_container_cmd" = "rm" ]; then
        remove_container
    else
        log_err "Container command \"$do_container_cmd\" is invalid."
        return 1
    fi
}
