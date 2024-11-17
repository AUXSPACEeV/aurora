#!/bin/bash
#
# Zephyr application docker entrypoint
#
# Author: Maximilian Stephan @ Auxspace e.V.
# Copyright (c) 2024 Auxspace e.V.
#

set -e

################################################################################
# Functions                                                                    #
################################################################################

function first_boot_actions() {
  local tmp_dir=$(dirname -- "$FIRST_BOOT_TMP_FLAG")

  cd /builder/zephyr-workspace

  # Update the west workspace
  # This step is done, since "west update" is only run in container
  # using the downloaded version of our application.
  # We later mount our own clone of the repo, so updating again
  # gets us the latest changes
  west update

  # Setup tmp_dir if not already done
  sudo mkdir -p "$tmp_dir"
  sudo chown -R builder:builder "$tmp_dir"

  # Signalize our success
  touch "${FIRST_BOOT_TMP_FLAG}"
}

# TODO: Build functions

function build_app() {
  echo "ERROR: Build function not implemented yet!"
}

function checkout_app() {
  echo "ERROR: Checkout function not implemented yet!"
}

function clean_app() {
  echo "ERROR: Clean function not implemented yet!"
}

################################################################################
# Variables                                                                    #
################################################################################

COMMAND="/bin/bash"
declare -a ARGS=()

# Flag that shows if everything has already been initialized.
FIRST_BOOT_TMP_FLAG="/var/auxspace/first_boot"

################################################################################
# Run first boot code to setup the environment correctly                       #
################################################################################

if [ ! -f "$FIRST_BOOT_TMP_FLAG" ]; then
  first_boot_actions
fi

################################################################################
# Commandline arg parser                                                       #
################################################################################

while [ $# -gt 0 ]; do
  case $1 in
    build)
      build_app
      exit 0
      ;;
    checkout)
      checkout_app
      exit 0
      ;;
    clean)
      clean_app
      exit 0
      ;;
    shell)
      COMMAND="/bin/bash"
      shift
      ;;
    --*)
      echo "ERROR: No such option: $1"
      exit 1
      ;;
    *)
      log_err "ERROR: No such command: ${COMMAND}"
      exit 1
      ;;
  esac
done

################################################################################
# Run command                                                                  #
################################################################################

$COMMAND ${ARGS[@]}
