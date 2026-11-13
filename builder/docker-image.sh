#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the toolchain image (switch/docker/Dockerfile) the Switch build runs in. Once.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
docker build -t "${ZNX_DOCKER_IMAGE:-z-cancel-nx-build:latest}" "$ROOT/switch/docker"
