#!/bin/env bash
#
# Zephyr application build script
#
# Author: Maximilian Stephan @ Auxspace e.V.
# Copyright (c) 2024 Auxspace e.V.
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
    echo "  $0 [OPTIONS] { build | checkout | cleanall | shell }"
    echo
    echo "Positional arguments:"
    echo "  build                          Build the platform image."
    echo "  checkout                       Checkout the layers and sources."
    echo "  clean                          Clean the build directory."
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
    echo "   --ssh-key PRIVATE_KEYFILE     Add a private key to your ssh-agent."
    echo "   --variant { dev | build }     Set the container variant."
    echo "                                 Defaults to $CONTAINER_VARIANT."
    echo "-v|--verbose                     Set DEBUG log level (+set -x)."
}

function check_and_build_container() {
    log_info "Checking for the ssh agent ..."
    if [ ! -S "${SSH_AUTH_SOCK}" ]; then
        log_err "SSH agent is not setup."
        log_warn "  > Use $0 --ssh-key PRIVATE_KEYFILE for a fast setup." 
        exit 1
    fi

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

function build_container {
    log_info "Building container variant '$CONTAINER_VARIANT' for '$CONTAINER_TAG' ..."
    if [ -z "${_CONTAINER_BUILD_BIN}" ]; then
        log_err "No container build command passed."
        exit 1
    fi

    DOCKER_BUILDKIT=1 ${_CONTAINER_BUILD_BIN} build \
        --build-arg PUID=$(id -u) \
        --build-arg PGID=$(id -g) \
        --build-arg ZEPHYR_SOURCE_SW_VERSION=${ZEPHYR_SOURCE_SW_VERSION} \
        --ssh default=${SSH_AUTH_SOCK} \
        --tag $CONTAINER_TAG \
        --file "$CONTAINER_DIR/$CONTAINER_VARIANT/Dockerfile" \
        "$CONTAINER_DIR"
}

function configure_ssh_agent() {
    log_info "Setting up the ssh agent ..."
    # Prompt the user for the path to the private SSH key
    local ssh_key_path="${1}"
    local ssh_key_path_default="${HOME/.ssh/id_rsa}"
    if [ -z "$ssh_key_path" ]; then
        log_info "Enter the path to your private SSH key."
        read -p "(default: $ssh_key_path_default): " ssh_key_path
    fi

    # Check if the file exists
    if [ ! -f "$ssh_key_path" ]; then
        log_err "File '$ssh_key_path' does not exist."
        exit 1
    fi
    
    SSH_PRIVATE_KEYFILE="$ssh_key_path"

    # Start the ssh-agent if it's not already running
    if [ ! -S "$SSH_AUTH_SOCK" ]; then
        eval "$(ssh-agent -s)"
    fi

    # Add the SSH key to the ssh-agent
    ssh-add "$ssh_key_path"

    log_info "SSH key added to the ssh-agent."
}

function start_container {
    log_info "Starting container $CONTAINER_NAME"
    $_CONTAINER_BIN start $CONTAINER_NAME
}

function run_container_cmd {
    local use_run_cmd="${1:-0}"
    if [ "$use_run_cmd" = "1" ]; then
        log_info "Running '$CONTAINER_TAG' ..."
        $_CONTAINER_BIN run \
            $CONTAINER_RUNTIME_ARGS \
            -v "${SSH_AUTH_SOCK}:/ssh-agent" \
            $CONTAINER_TAG \
            $COMMAND
    else
        log_info "Attaching to running container '$CONTAINER_NAME' ..."
        $_CONTAINER_BIN exec -it \
            $CONTAINER_NAME \
            /builder/container-entrypoint.sh $COMMAND
    fi
}

function run_container() {
    local use_run_cmd="0"
    if ! ${_CONTAINER_BIN} container ls -a --format '{{.Names}}' \
        | grep -q "$CONTAINER_NAME"; then
        log_warn "Container '$CONTAINER_NAME' does not exist. Using 'run' command."
        use_run_cmd="1"
    elif ! ${_CONTAINER_BIN} ps --format '{{.Names}}' \
        | grep -q "$CONTAINER_NAME"; then
        log_warn "Container '$CONTAINER_NAME' is stopped. Using 'start' command."
        start_container
    fi

    run_container_cmd $use_run_cmd
}

################################################################################
# Variables                                                                    #
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
CONTAINER_NAME="auxspace-zephyr-builder"
CONTAINER_TAG="${CONTAINER_NAME}:local"
CONTAINER_VARIANT="dev"
CONTAINER_ENGINE="docker"
_CONTAINER_BIN="docker"
_CONTAINER_BUILD_BIN="docker buildx"
ZEPHYR_SOURCE_SW_VERSION="main"

# SSH
SSH_DIR="${CONTAINER_DIR}/.ssh"

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
        --ssh-key)
            SSH_PRIVATE_KEYFILE="$2"
            shift 2
            ;;
        --variant)
            CONTAINER_VARIANT="$2"
            shift 2
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
                build|shell|cleanall|checkout)
                    shift
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
    --user $(id -u):$(id -g) \
    -v "${THISDIR}:/workdir:rw" \
    -e SSH_AUTH_SOCK=/ssh-agent \
    --workdir /workdir \
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

if [ -n "${SSH_PRIVATE_KEYFILE}" ]; then
    configure_ssh_agent $SSH_PRIVATE_KEYFILE
fi

check_and_build_container

run_container
