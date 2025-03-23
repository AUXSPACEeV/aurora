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
        log_warning "No python venv found."
        log_warning "Make sure to setup the environment first!"
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
