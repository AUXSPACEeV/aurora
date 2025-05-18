#syntax=docker/dockerfile:1.0.0-experimental
#escape=\
#
# Auxspace development container for AURORA.
#
# Author: Maximilian Stephan @ Auxspace e.V.
# Copyright (c) 2025 Auxspace e.V.
#

# digests to FROM debian:trixie-slim
FROM debian@sha256:1f87f0180766b28b7834a0b5f134948f9c2fea18ffa09bd62a7c93cc66ef5d99

ARG PUID=10001
ARG PGID=10001

# Debian container fixes
ENV DEBIAN_FRONTEND=noninteractive

# Home directory for builder user
ENV BUILDER_HOME="/builder"

# Path definitions
ENV BUILD_DEPS_PATH="${BUILDER_HOME}/deps"
ENV PICOTOOL_PATH="${BUILD_DEPS_PATH}/picotool"
ENV PICO_PROJECT_GENERATOR_PATH="${BUILD_DEPS_PATH}/pico-project-generator"

# Color prompt
ENV color_prompt="yes"

# Update APT index + setup and install locales
RUN apt-get update && \
    apt-get install -y --no-install-recommends locales && \
    localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

# pico sdk dependencies
RUN apt-get install -y --no-install-recommends \
    cmake python3 build-essential gcc-arm-none-eabi libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib

# picotool dependencies
RUN apt-get install -y --no-install-recommends \
    build-essential pkg-config libusb-1.0-0-dev cmake

# pico project generator dependencies
RUN apt-get install -y --no-install-recommends \
    python3 python3-dev python3-tk python3-venv python3-setuptools

# documentation dependencies
RUN apt-get install -y --no-install-recommends \
    python3-sphinx python3-pip

# Other packages required in the container + development tools
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    gosu sudo openssh-client less neovim file strace git-core ca-certificates \
    ninja-build

# Setup the container builder user
RUN groupadd --gid ${PGID} builder
RUN useradd --create-home \
    --shell /bin/bash \
    --home-dir ${BUILDER_HOME} \
    --uid ${PUID} \
    --gid ${PGID} \
    --groups sudo,dialout,audio,video,disk \
    builder
RUN echo "builder  ALL=(ALL:ALL) NOPASSWD:ALL" > /etc/sudoers.d/builder-nopasswd
RUN echo 'Defaults env_keep += "ftp_proxy http_proxy https_proxy no_proxy"' \
    >> /etc/sudoers.d/builder-nopasswd

# picotool installation
RUN git clone https://github.com/raspberrypi/picotool.git ${PICOTOOL_PATH}

# pico project generator installation
RUN git clone https://github.com/raspberrypi/pico-project-generator.git ${PICO_PROJECT_GENERATOR_PATH}

# Configure user
RUN mkdir -p -m 0700 ${BUILDER_HOME}/.ssh && ssh-keyscan github.com \
    >> ${BUILDER_HOME}/.ssh/known_hosts

# Bash aliases
COPY bash_aliases ${BUILDER_HOME}/.bash_aliases

# Change permissions for the builder user
RUN chown -R ${PUID}:${PGID} ${BUILDER_HOME}

# Docker entrypoint
COPY entrypoint /sbin/entrypoint

ENTRYPOINT [ "/sbin/entrypoint" ]