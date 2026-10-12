# CoD2x Linux dedicated server
#
# Build:  docker build -t cod2x:latest .
# Run:    docker compose up -d   (see compose.yaml and the README)
#
# Ubuntu 18.04 is the same environment as the official Linux build and still provides
# libstdc++5:i386, which the original cod2_lnxded needs.

# --- Build libCoD2x.so ---
FROM ubuntu:18.04 AS build

RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends make cmake ninja-build g++-multilib libssl-dev:i386 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/CoD2x
COPY CMakeLists.txt makefile ./
COPY src ./src
RUN mkdir -p bin/linux && make build_linux DEBUG=0


# --- Server image ---
FROM ubuntu:18.04

RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends libc6-i386 libstdc++5:i386 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /home/cod2
ENV HOME=/home/cod2

# Original CoD2 1.3 dedicated server + CoD2x
COPY bin/linux/cod2_lnxded ./cod2_lnxded
COPY --from=build /tmp/CoD2x/bin/linux/libCoD2x.so ./libCoD2x.so
RUN chmod +x cod2_lnxded libCoD2x.so

# Server log (fs_homepath is ~/.callofduty2) goes to the container output
RUN mkdir -p .callofduty2/main \
 && ln -sf /dev/stdout .callofduty2/main/games_mp.log

# Game files (original CoD2 .iwd files, server.cfg, mods) are mounted here
VOLUME [ "/home/cod2/main" ]

# Game port (UDP)
EXPOSE 28960/udp

# LD_PRELOAD is set only for the server process, not for the whole container
ENTRYPOINT [ "env", "LD_PRELOAD=/home/cod2/libCoD2x.so", "./cod2_lnxded" ]
CMD [ "+set", "dedicated", "2", "+set", "net_port", "28960", "+exec", "server.cfg" ]
