# wetrix-arm64:noble - the aarch64 toolchain for the Wetrix Linux ARM build.
#
# build.sh builds it automatically when the image is absent:
#   docker build --platform linux/arm64 -t wetrix-arm64:noble -f Dockerfile .
#
# Ubuntu 24.04 (gcc 13, glibc 2.39, SDL2 2.30). No ninja: the build uses Makefiles.
# libsdl2-dev pulls in the GL/GLES/EGL headers. The device's glibc is newer than
# this image's, which is what makes the binary loadable there.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends \
    build-essential cmake pkg-config git ca-certificates python3 libsdl2-dev \
 && rm -rf /var/lib/apt/lists/*
