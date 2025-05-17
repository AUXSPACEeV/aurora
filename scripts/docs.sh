#!/bin/env bash
#
# Auxspace avionics application documentation wrapper
#
# This script wraps the dependency management and the documentation build with
# Sphinx.
#
# Author: Maximilian Stephan @ Auxspace e.V.
# Copyright (c) 2025 Auxspace e.V.
#

set -e

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

source "$THISDIR/lib/log.sh"

#default docs dir
DOCS_SRC_DIR="${DOCS_SRC_DIR:-$THISDIR/../docs}"

################################################################################
# Functions                                                                    #
################################################################################

function print_help() {
    echo "Auxspace AURORA documentation wrapper."
    echo
    echo "Usage:"
    echo "  $0 [OPTIONS] {setup | SPHINX_ARGUMENTS}"
    echo
    echo "Positional Arguments:"
    echo "  setup                  Set the environment up before building."
    echo "  <SPHINX_ARGUMENTS>     Args to run \"make\" in sphinx src dir with."
    echo
    echo "Options":
    echo "-s|--src-dir DIR         Documentation source directory."
    echo "                         Defaults to \"$DOCS_SRC_DIR\"."
    echo "-h|--help                Print this help text."
    echo "-l|--log-level LEVEL     Set log level to LEVEL."
    echo "                         -1 -> logging OFF"
    echo "                         $LOG_LEVEL_ERR  -> ERROR"
    echo "                         $LOG_LEVEL_WARN  -> WARN"
    echo "                         $LOG_LEVEL_INFO  -> INFO"
    echo "                         $LOG_LEVEL_DEBUG  -> DEBUG"
    echo "-v|--verbose             Set DEBUG log level (+set -x)."
}

function docs_setup() {
    set -e
    log_info "Setting up documentation environment in \"$DOCS_SRC_DIR\" ..."
    if [ ! -f "$DOCS_SRC_DIR/.venv/bin/activate" ]; then
        log_info "Building python3 venv ..."
        python3 -m venv "$DOCS_SRC_DIR/.venv"
    fi

    source "$DOCS_SRC_DIR/.venv/bin/activate"

    log_info "Fetching pip dependencies ..."
    pip install -r "$DOCS_SRC_DIR/requirements.txt"

    log_info "Documentation setup done!"
}

function sphinx_cmd() {
    set -e
    if [ ! -f "$DOCS_SRC_DIR/.venv/bin/activate" ]; then
        log_warn "No python venv found."
        log_warn "Make sure to setup the environment first!"
    else
        source "$DOCS_SRC_DIR/.venv/bin/activate"
    fi
    log_info "Running sphinx ..."
    log_debug "command: make $@"
    cd $DOCS_SRC_DIR && make $@
}

################################################################################
# Commandline arg parser                                                       #
################################################################################

while [ $# -gt 0 ]; do
    case $1 in
        --help)
            print_help
            exit 0
            ;;
        --src-dir|-s)
            DOCS_SRC_DIR="$2"
            shift 2
            ;;
        setup)
            docs_setup 
            shift
            ;;
        *)
            sphinx_cmd $@
            exit 0
            ;;
    esac
done
