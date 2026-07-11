ARG BASE_IMAGE=ubuntu:22.04

FROM ${BASE_IMAGE} AS muduo-builder

ARG DEBIAN_FRONTEND=noninteractive
ARG MUDUO_TARBALL_URL=https://github.com/chenshuo/muduo/archive/f1fc77e0c13b80e5086ff457362c8a86d1b609d4.tar.gz

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        cmake \
        build-essential \
        libboost-dev \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL --retry 3 --retry-delay 2 -o /tmp/muduo.tar.gz "${MUDUO_TARBALL_URL}" \
    && mkdir -p /tmp/muduo-src \
    && tar -xzf /tmp/muduo.tar.gz -C /tmp/muduo-src --strip-components=1 \
    && cmake -S /tmp/muduo-src -B /tmp/muduo-build \
        -DCMAKE_BUILD_TYPE=Release \
        -DMUDUO_BUILD_EXAMPLES=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    && cmake --build /tmp/muduo-build --target muduo_base muduo_net -j"$(nproc)" \
    && mkdir -p /opt/muduo/include /opt/muduo/lib \
    && cp /tmp/muduo-build/lib/libmuduo_base.a /tmp/muduo-build/lib/libmuduo_net.a /opt/muduo/lib/ \
    && cp -R /tmp/muduo-src/muduo /opt/muduo/include/

FROM muduo-builder AS builder

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libssl-dev \
        libmysqlclient-dev \
        libmysqlcppconn-dev \
        libhiredis-dev \
    && cp -R /opt/muduo/include/muduo /usr/local/include/muduo \
    && cp /opt/muduo/lib/libmuduo_*.a /usr/local/lib/ \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY HttpServer ./HttpServer
COPY apps ./apps

RUN cmake -S /src -B /tmp/haoHTTP-build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
    && cmake --build /tmp/haoHTTP-build --target shortlink_server -j"$(nproc)" \
    && install -Dm755 /tmp/haoHTTP-build/shortlink_server /opt/hao-shortlink/shortlink_server \
    && mkdir -p /opt/hao-shortlink/apps/shortlink_server \
    && cp -R /src/apps/shortlink_server/config /opt/hao-shortlink/apps/shortlink_server/config

FROM ${BASE_IMAGE} AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        libssl3 \
        libmysqlclient21 \
        libmysqlcppconn7v5 \
        libhiredis0.14 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/hao-shortlink

COPY --from=builder /opt/hao-shortlink/ /opt/hao-shortlink/

EXPOSE 8080

HEALTHCHECK --interval=5s --timeout=3s --retries=20 \
    CMD curl -fsS http://127.0.0.1:8080/api/health >/dev/null || exit 1

CMD ["./shortlink_server", "apps/shortlink_server/config/server.container.conf.example"]
