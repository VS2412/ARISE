# ╔══════════════════════════════════════════════════════════════════════════╗
# ║               ARISE — Arch Linux Build & Test Container                  ║
# ╚══════════════════════════════════════════════════════════════════════════╝
#
# Mirrors the exact target OS. Builds the project and runs all GTest suites.
#
# REPRODUCIBILITY NOTE:
#   The base image is pinned to a specific digest. This prevents a change in
#   archlinux:latest from silently breaking the build between two identical
#   source commits. To intentionally update the base image, pull the new
#   digest with:
#       docker pull archlinux:latest
#       docker image inspect archlinux:latest --format '{{index .RepoDigests 0}}'
#   and update the digest below.
#
# Usage:
#   docker build -t arise-test .
#   docker run --rm arise-test            # runs ctest automatically
#   docker run --rm -it arise-test bash   # drop into a shell for exploration
#   docker compose up --build test        # convenience wrapper

# Pinned to the amd64 digest for archlinux:latest pushed on 2026-09-21.
# To update: docker pull archlinux:latest && docker image inspect archlinux:latest --format "{{index .RepoDigests 0}}"
FROM archlinux@sha256:917e543c9d0f1f495d70907bdf05bf53607e791b351e1b01ccd3aec2442303ed

LABEL maintainer="ARISE Project"
LABEL description="Arch Linux build and test environment for ARISE"

# ── 1. Refresh package database and install exact build dependencies ──────────
# We do NOT run `pacman -Syu` (full system upgrade) because:
#   - It pulls in unrelated package changes not needed for the build.
#   - It can fail due to keyring or mirror sync issues in rolling releases.
# Instead, we install only the packages we explicitly need.
# ARG CACHE_DATE can be set to bust this layer intentionally, e.g.:
#   docker build --build-arg CACHE_DATE=$(date +%Y-%m-%d) -t arise-test .
ARG CACHE_DATE=2024-12-01
RUN pacman-key --init && \
    pacman-key --populate archlinux && \
    pacman -Sy --noconfirm \
        base-devel \
        cmake \
        git \
        curl \
        python \
        nlohmann-json \
        alsa-lib \
        portaudio \
    && pacman -Scc --noconfirm

# ── 2. Copy project source ────────────────────────────────────────────────────
WORKDIR /arise
COPY . .

# ── 3. Configure and build (CMake fetches whisper.cpp & GTest automatically) ──
# WHISPER_DIR is left at its default (OFF) so FetchContent is always used in CI,
# guaranteeing the pinned v1.7.1 dependency regardless of the host machine.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DWHISPER_DIR=OFF \
    && cmake --build build --parallel "$(nproc)"

# ── 4. Default command: run all unit tests via CTest ──────────────────────────
WORKDIR /arise/build
CMD ["ctest", "--output-on-failure", "--test-dir", "."]

