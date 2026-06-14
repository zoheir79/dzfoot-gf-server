# Build stage
FROM ubuntu:22.04 AS builder

ARG CACHEBUST=0
RUN apt-get update && apt-get install -y \
    build-essential git pkg-config ninja-build \
    libcurl4-openssl-dev libssl-dev libhiredis-dev \
    libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev libsdl2-gfx-dev \
    libopenal-dev libegl1-mesa-dev libgl1-mesa-dev \
    python3-dev python3-pip \
    libboost-python-dev libboost-thread-dev libboost-filesystem-dev libboost-system-dev \
    libprotobuf-dev protobuf-compiler libabsl-dev libsqlite3-dev \
    cmake ca-certificates curl wget xz-utils make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

# Build GameplayFootball + gf_server
RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release \
      -DEGL_INCLUDE_DIR=/usr/include/EGL \
      -DEGL_LIBRARY=/usr/lib/x86_64-linux-gnu/libEGL.so
RUN bash -c 'set -o pipefail; cd build && make -j1 2>&1 | tee /tmp/build.log; exit_code=${PIPESTATUS[0]}; cp /tmp/build.log /build/build.log; if [ $exit_code -ne 0 ]; then cat /tmp/build.log; exit $exit_code; fi; ls -la gf_server'

# Runtime stage
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libcurl4 libssl3 libhiredis0.14 \
    libsdl2-2.0-0 libsdl2-image-2.0-0 libsdl2-ttf-2.0-0 libsdl2-gfx-1.0-0 \
    libopenal1 libegl1 libgl1 \
    python3 libpython3.10 \
    libboost-filesystem1.74.0 libboost-system1.74.0 libboost-thread1.74.0 libboost-python1.74.0 \
    libprotobuf23 libabsl20210324 \
    fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app/data
COPY --from=builder /build/build/gf_server /usr/local/bin/gf_server
COPY --from=builder /build/build/GF_build/libgame.so /usr/local/lib/
COPY --from=builder /build/GameplayFootball/data /app/data
RUN ldconfig

# gf_server communicates via Redis only.
# Game states are published to Redis; backend session bot forwards to LiveKit.
# Player inputs are received via Redis subscription.

ENTRYPOINT ["gf_server"]
