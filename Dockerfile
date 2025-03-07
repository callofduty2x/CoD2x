FROM ubuntu:latest AS build

RUN dpkg --add-architecture i386
RUN apt update
RUN apt install -y make g++-multilib libc6-dev-i386 libstdc++5:i386

RUN mkdir -p "/tmp/CoD2x/src"
RUN mkdir -p "/tmp/CoD2x/bin/linux"
COPY src "/tmp/CoD2x/src"
COPY makefile "/tmp/CoD2x"
COPY "bin/linux/cod2_lnxded_crack" "/tmp/CoD2x/bin/linux"
WORKDIR /tmp/CoD2x

RUN make build_cod2x_linux

FROM ubuntu:latest
RUN dpkg --add-architecture i386
RUN apt update
RUN apt install -y libc6-i386 libstdc++5:i386

COPY --from=build /tmp/CoD2x/bin/linux/libCoD2x.so /lib/libCoD2x.so
COPY --from=build /tmp/CoD2x/bin/linux/cod2_lnxded_crack /home/cod2/cod2_lnxded
RUN chmod +x /home/cod2/cod2_lnxded

EXPOSE 20500/udp 20510/udp 28960/tcp 28960/udp

VOLUME [ "/home/cod2/main" ]

WORKDIR /home/cod2

RUN mkdir -p /home/cod2/.callofduty2/main/ \
  && ln -sf /dev/stdout /home/cod2/.callofduty2/main/games_mp.log

ENV LD_PRELOAD="/lib/libCoD2x.so"
ENTRYPOINT [ "./cod2_lnxded" ]