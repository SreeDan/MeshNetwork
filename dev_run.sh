#!/bin/bash

PROJECT_ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="$PROJECT_ROOT/build"
SSL_CONFIG="$PROJECT_ROOT/certs/openssl.cnf"
DEBUG=false

while getopts "dn:v" flag; do
    case "${flag}" in
        d)
            DEBUG=true
            ;;
        *)
            echo "Invalid option: -${OPTARG}" >&2
            exit 1
            ;;
    esac
done

shift $((OPTIND-1))

export OPENSSL_CONF="$SSL_CONFIG"

echo "--- Building ---"

if [ "$DEBUG" = "true" ]; then
    BUILD_SCOPE="debug"
    make build debug
else
    BUILD_SCOPE="release"
    make build
fi

if [ $? -ne 0 ]; then
    echo "❌ Build failed. Aborting run."
    exit 1
fi

BINARY="$BUILD_DIR/$BUILD_SCOPE/MeshNetworking"

echo "--- Running ---"
if [ ! -f "$BINARY" ]; then
    echo "❌ Binary not found at: $BINARY"
    exit 1
fi

if [ "$DEBUG" = "true" ]; then
    echo "Starting GDB..."
    gdb --args "$BINARY" "$@"
else
    "$BINARY" "$@"
fi