# Build stage
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential git pkg-config ninja-build \
    libcurl4-openssl-dev libssl-dev libhiredis-dev \
    libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev libsdl2-gfx-dev \
    libopenal-dev libegl1-mesa-dev libgl1-mesa-dev \
    python3-dev python3-pip \
    libboost-python-dev libboost-thread-dev libboost-filesystem-dev libboost-system-dev \
    libprotobuf-dev protobuf-compiler libabsl-dev \
    cmake ca-certificates curl wget xz-utils make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
ARG CACHEBUST=0
COPY . .

# Build GameplayFootball + gf_server
RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release \
      -DEGL_INCLUDE_DIR=/usr/include/EGL \
      -DEGL_LIBRARY=/usr/lib/x86_64-linux-gnu/libEGL.so && \
    make -j$(nproc)

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

WORKDIR /app
COPY --from=builder /build/build/gf_server /usr/local/bin/gf_server
COPY --from=builder /build/build/GF_build/libgame.so /usr/local/lib/
COPY --from=builder /build/build/_deps/libdatachannel-build/libdatachannel.so* /usr/local/lib/
COPY --from=builder /build/GameplayFootball/data /app/data
RUN ldconfig

# Note: no EXPOSE needed — WebRTC data channels use ephemeral UDP ports.
# LiveKit/libdatachannel handles NAT traversal (ICE/STUN).

ENTRYPOINT ["gf_server"]
