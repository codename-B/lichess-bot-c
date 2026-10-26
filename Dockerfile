# syntax=docker/dockerfile:1
FROM debian:bookworm-slim AS build
ARG BUILD_JOBS=2
ARG CFLAGS="-O3 -DNDEBUG"
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential libcurl4-openssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY . .
RUN mkdir /source \
    && cp -a src include vendor examples tests docs \
        Makefile Dockerfile docker-compose.yml LICENSE README.md config.yml.default /source/ \
    && make -j"${BUILD_JOBS}" CFLAGS="${CFLAGS}" \
    && make install DESTDIR=/install PREFIX=/usr/local CFLAGS="${CFLAGS}" \
    && ./lichess-bot-c --version

FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
        libcurl4 ca-certificates libstdc++6 libgomp1 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 bot \
    && useradd --uid 10001 --gid 10001 --home-dir /data --no-create-home \
        --shell /usr/sbin/nologin bot \
    && mkdir -p /data /config /engines /books /tablebases \
    && chown bot:bot /data
COPY --from=build /install/ /
COPY --from=build /source/ /usr/share/lichess-bot-c/source/
WORKDIR /data
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/lichess-bot-c"]
CMD ["--config", "/config/config.yml"]
