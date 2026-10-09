#!/bin/sh
# Build inside a throwaway Debian container, nothing installed on the host:
#   docker run --rm -v "$PWD":/src -w /src debian:stable-slim sh build-in-docker.sh
set -e
apt-get update -qq && apt-get install -y -qq gcc-mingw-w64-i686 python3 >/dev/null
sh build.sh
