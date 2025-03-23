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
declare -r _CONTAINER_TAG="${_CONTAINER_NAME}:local"

# global r/w for container config
declare -x CONTAINER_DIR="${THISDIR}/container"
declare -x CONTAINER_ENGINE="docker"
declare -x CONTAINER_BIN="docker"
declare -x CONTAINER_BUILD_BIN="${CONTAINER_BIN} buildx"
declare -x -i REBUILD_CONTAINER_IMAGE=0

# this should only be appended and not overwritten
declare -x CONTAINER_RUNTIME_ARGS=" \
    -it \
    --name ${_CONTAINER_NAME} \
    -e PUID=`id -u` \
    -e PGID=`id -g` \
    --user $(id -u):$(id -g) \
"

function check_and_build_container() {
    log_info "Checking container engine ..."
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
        log_info "Image for container '$_CONTAINER_TAG' already exists."
        log_info "Skipping container build."
    fi
}

function build_container() {
    log_info "Building container '$_CONTAINER_TAG' ..."
    if [ -z "${CONTAINER_BUILD_BIN}" ]; then
        log_err "No container build command passed."
        exit 1
    fi

    DOCKER_BUILDKIT=1 ${CONTAINER_BUILD_BIN} build \
        --build-arg PUID=$(id -u) \
        --build-arg PGID=$(id -g) \
        --tag $_CONTAINER_TAG \
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
        $CONTAINER_BIN image rm ${_CONTAINER_TAG}
    fi

    log_info "Container has been removed."
}

function start_container() {
    log_info "Starting container $_CONTAINER_NAME"
    $CONTAINER_BIN start $_CONTAINER_NAME
}

function run_container_cmd() {
    local use_run_cmd="${1:-0}"
    if [ "$use_run_cmd" = "1" ]; then
        log_info "Running '$_CONTAINER_TAG' ..."
        $CONTAINER_BIN run \
            $CONTAINER_RUNTIME_ARGS \
            $_CONTAINER_TAG
    fi

    if ! ${CONTAINER_BIN} ps --format '{{.Names}}' \
        | grep -q "$_CONTAINER_NAME"; then
        log_warn "Container '$_CONTAINER_NAME' is stopped. Using 'start'."
        start_container
    fi

    log_info "Attaching to running container '$_CONTAINER_NAME' ..."
    $CONTAINER_BIN exec -it \
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
    if [ "$do_container_cmd" = "build" ]; then
        check_and_build_container
    elif [ "$do_container_cmd" = "rm" ]; then
        remove_container
    else
        log_err "Container command \"$do_container_cmd\" is invalid."
        return 1
    fi
}