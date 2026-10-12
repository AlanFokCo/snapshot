#!/bin/sh
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Prints the buildx cache flags for one image, on one branch.
#
# The cache is keyed by branch. main and release/* both publish artifacts, and
# an unkeyed tag meant a release push overwrote the cache every pull request
# against main reads, sending the next agent build cold for no reason the
# author could see.
#
# Usage: ci-cache-flags.sh <read|write> <image> <branch>
#
#   read   one --cache-from for the branch, and one for main behind it, so a
#          pull request against a release branch still starts from main's
#          layers when its own tag is missing or new. Pull requests never
#          write: a cache a pull request can write is a cache a fork can
#          poison.
#   write  --cache-from and --cache-to for the branch. Only main and release/*
#          reach this, from push-artifacts.yaml.
#
# Reads CACHE_REF_PREFIX, and CACHE_TAG_SUFFIX if set — any unused value there
# points every flag at a tag that does not exist, which is how a cold build is
# measured without disturbing the cache other runs are reading.

set -eu

MODE=${1:?usage: ci-cache-flags.sh <read|write> <image> <branch>}
IMAGE=${2:?usage: ci-cache-flags.sh <read|write> <image> <branch>}
BRANCH=${3:?usage: ci-cache-flags.sh <read|write> <image> <branch>}

: "${CACHE_REF_PREFIX:?CACHE_REF_PREFIX must name the cache repository}"
SUFFIX=${CACHE_TAG_SUFFIX:-}

# Same shape as the image tag slug in push-artifacts.yaml: lowercase
# alphanumerics and single hyphens. A tag cannot hold the slash in release/0.1.
slug() {
    printf '%s' "$1" |
        tr '[:upper:]' '[:lower:]' |
        sed -E 's/[^a-z0-9]+/-/g; s/^-+//; s/-+$//'
}

branch_slug=$(slug "$BRANCH")
branch_slug=${branch_slug:-branch}

ref="${CACHE_REF_PREFIX}:${IMAGE}-${branch_slug}${SUFFIX}"
main_ref="${CACHE_REF_PREFIX}:${IMAGE}-main${SUFFIX}"

case "$MODE" in
read)
    printf '%s' "--cache-from=type=registry,ref=${ref}"
    # Nothing to fall back to when the branch is main, and importing the same
    # cache twice is not free.
    if [ "$ref" != "$main_ref" ]; then
        printf ' %s' "--cache-from=type=registry,ref=${main_ref}"
    fi
    ;;
write)
    # image-manifest and oci-mediatypes because GHCR rejects buildkit's
    # default cache manifest type.
    printf '%s %s' \
        "--cache-from=type=registry,ref=${ref}" \
        "--cache-to=type=registry,ref=${ref},mode=max,image-manifest=true,oci-mediatypes=true"
    ;;
*)
    echo "unknown mode: $MODE (expected read or write)" >&2
    exit 2
    ;;
esac
