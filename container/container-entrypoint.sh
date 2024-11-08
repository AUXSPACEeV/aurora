#!/bin/bash

set -e

COMMAND="/bin/bash"
declare -a ARGS=()

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

$COMMAND ${ARGS[@]}
