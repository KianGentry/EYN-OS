#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="eyn-os"
CONTAINER_RUNTIME="${CONTAINER_RUNTIME:-docker}"

# Detect podman fallback if docker not present
if ! command -v "$CONTAINER_RUNTIME" >/dev/null 2>&1; then
    if command -v podman >/dev/null 2>&1; then
        CONTAINER_RUNTIME="podman"
    else
        echo "Error: neither docker nor podman found"
        exit 1
    fi
fi

# Build image if it doesn't exist
if ! "$CONTAINER_RUNTIME" image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    echo "[dev.sh] Building container image..."
    "$CONTAINER_RUNTIME" build -t "$IMAGE_NAME" .
fi

# Handle TTY properly (important for make + qemu)
TTY_FLAGS=""
if [ -t 1 ]; then
    TTY_FLAGS="-it"
fi

exec "$CONTAINER_RUNTIME" run --rm \
    $TTY_FLAGS \
    -u "$(id -u):$(id -g)" \
    -v "$(pwd)":/workspace:Z \
    -w /workspace \
    -p 8080:1234 \
    "$IMAGE_NAME" \
    make "$@"