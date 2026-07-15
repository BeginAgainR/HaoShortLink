# 运行手册

状态：已建立本地运行手册，持续维护
当前实现：已整理 Linux VM 手工构建、配置和运行方式；当前阶段不在 Mac 宿主机上构建或运行。

## 本地 Mac 宿主机

当前 Mac 宿主机仅用于：

- 编辑代码和文档。
- 查看 git 状态和 diff。
- 维护项目文档。

当前 Mac 宿主机不用于：

- 构建项目。
- 运行服务。
- 生成 `build/` 目录。
- 执行压测。

## Linux 验证环境

构建和运行验证后续在 Linux 虚拟机或容器环境中进行。

当前本地验证环境：

- OrbStack Linux VM：`haoHTTP`
- 进入方式：`ssh haoHTTP@orb`
- 项目路径：`/Users/hao/Code/haoHTTP`
- 当前分支：`codex/v1.6-reliability`

当前 VM 已安装：

- 基础构建工具：`build-essential`、`cmake`、`git`、`pkg-config`
- 开发依赖：`libssl-dev`、`libmysqlclient-dev`、`libmysqlcppconn-dev`、`libhiredis-dev`
- muduo 核心库：`muduo_base`、`muduo_net`

本地 muduo 安装位置：

```text
/opt/muduo-src
/usr/local/include/muduo
/usr/local/lib/libmuduo_base.a
/usr/local/lib/libmuduo_net.a
```

说明：

- HaoShortLink 当前只依赖 `muduo_base` 和 `muduo_net`。
- muduo 自带的可选 `muduo_http` 组件不是当前项目依赖。
- muduo 全量构建在 GCC 15 下可能因 `-Werror` 将警告提升为错误；当前只安装项目需要的核心库。

## 开发与验证工作流

日常开发采用：

```text
Mac 本地编辑代码和管理 git
OrbStack Linux VM 构建、运行和验证
```

说明：

- 项目目录由 VM 直接访问 Mac 上的工作区。
- 构建命令必须在 Linux VM 中执行。
- Mac 宿主机只负责编辑、查看 diff 和提交。
- 构建产物不得提交到仓库。
- 日常构建目录使用 `/tmp/haoHTTP-build`，不要在仓库内生成 `build/`。

## 验证方式

### 日常挂载验证

日常开发优先使用挂载目录验证：

```text
ssh haoHTTP@orb
cd /Users/hao/Code/haoHTTP
```

这种方式适合频繁迭代，可以直接验证当前工作区中的改动。

### 干净克隆验证

阶段性验证可以使用从远端仓库重新克隆，或从已提交的本地仓库重新克隆的方式。

适用场景：

- 每个版本节点完成前，例如 v0.2、v1.0。
- 修改 `CMakeLists.txt`、依赖、构建脚本或 `.gitignore` 后。
- 引入 MySQL、Redis、Docker Compose、Nginx 等外部组件前后。
- 准备合并分支、发布版本或做最终确认前。
- 怀疑当前工作区依赖了未提交文件时。

干净克隆验证更接近 CI，因为它从远端或已提交仓库重新拉取代码，可以发现漏提交文件、本地隐藏依赖和
`.gitignore` 配置问题。

## 配置文件

当前配置加载使用简单 `key=value` 文本格式。

示例文件：

```text
apps/shortlink_server/config/server.conf.example
apps/shortlink_server/config/server.compose.conf.example
apps/shortlink_server/config/server.container.conf.example
```

示例内容：

```text
server.name=HaoShortLink
server.port=8080
server.thread_num=4
log.level=INFO
metrics.enabled=true
storage.type=memory
redis.enabled=false
rate_limit.enabled=false
```

说明：

- 空行和以 `#` 开头的整行注释会被忽略。
- key 和 value 两侧空白会被裁剪。
- 服务启动时可以传入配置文件路径；未传入时默认读取示例配置文件。
- `storage.type` 当前支持 `memory` 和 `mysql`。
- `memory` 是默认存储方式。
- `mysql` 需要先创建 `short_links` 表并配置 MySQL 连接信息。
- `redis.enabled` 是 MySQL 存储模式下的可选查询缓存开关，默认关闭。
- `metrics.enabled` 控制直连服务的 `GET /metrics` 路由，默认启用。
- `rate_limit.enabled` 控制 `POST /api/short-links` 的全局 Redis 固定窗口限流，默认关闭。
- `rate_limit.requests`、`rate_limit.window_seconds` 分别配置窗口额度和秒数。
- `rate_limit.key_prefix` 必须和 `redis.key_prefix` 不同。
- 限流与查询缓存开关独立，Redis 限流不可用时采用 fail-open。

健康检查：

```text
GET /api/health        兼容 liveness
GET /api/health/live   进程存活，不探测外部依赖
GET /api/health/ready  业务就绪，MySQL 模式下探测 MySQL
```

MySQL 存储模式下，MySQL 不可用时 readiness 返回 `503`，liveness 仍返回 `200`。
Redis 查询缓存或 Redis 限流故障不单独改变 readiness。

建议：

- 本地调试时可以复制示例文件为临时配置文件，并按环境修改数据库密码等敏感信息。
- 不要把包含真实密码的个人配置提交到仓库。
- `server.compose.conf.example` 用于 `shortlink_server` 在 `haoHTTP` VM 中运行、连接仓库根目录 `compose.yaml` 启动的 MySQL 和 Redis。
- `server.container.conf.example` 用于 `shortlink_server` 在 Docker Compose 容器内运行、通过服务名连接 MySQL 和 Redis。
- Nginx 本地反向代理配置位于 `deploy/nginx/default.conf`。
- 线上配置模板尚未接入。

## Docker Compose 本地运行

当前 `compose.yaml` 编排本地开发环境：

- MySQL
- Redis
- `shortlink_server`
- Nginx
- Prometheus
- Grafana

当前不编排：

- CI 或压测环境

运行位置：

- MySQL、Redis、`shortlink_server`、Nginx、Prometheus 和 Grafana 容器运行在 OrbStack Docker 中。
- Nginx 容器通过 Compose 服务名 `shortlink_server` 访问业务服务。
- `shortlink_server` 容器通过 Compose 服务名 `mysql`、`redis` 访问依赖。
- Prometheus 通过 Compose 服务名 `shortlink_server` 抓取 `/metrics`。
- Grafana 通过 Compose 服务名 `prometheus` 查询指标。
- 如果在 `haoHTTP` VM 中手工运行 `shortlink_server`，访问 OrbStack Docker 发布端口时使用 `docker.orb.internal`。
- 如果在 `haoHTTP` VM 中做 Redis 性能诊断，`docker.orb.internal` 或显式 IPv4 可能触发本地 OrbStack 网络慢路径；
  可用 `getent ahosts docker.orb.internal` 查看当前 IPv6 地址，并用该 IPv6 字面地址临时验证 Redis 延迟。
- 不需要在 `haoHTTP` Linux VM 内安装 Docker。

端口约定：

```text
Nginx 容器端口 80 -> OrbStack Docker 发布端口 8080
shortlink_server 容器端口 8080 -> OrbStack Docker 发布端口 18080
MySQL 容器端口 3306 -> OrbStack Docker 发布端口 13306
Redis 容器端口 6379 -> OrbStack Docker 发布端口 16379
Prometheus 容器端口 9090 -> 127.0.0.1:9090
Grafana 容器端口 3000 -> 127.0.0.1:3000
```

说明：

- 使用 `8080` 作为 Nginx 本地统一 HTTP 入口。
- `18080` 保留为 `shortlink_server` 直连调试入口。
- 使用 `13306` 和 `16379` 是为了避免和本地已有 MySQL、Redis 默认端口冲突。
- Prometheus 和 Grafana 只绑定 `127.0.0.1`，用于本机开发观察，不作为公开入口。
- MySQL 容器第一次初始化空数据目录时，会按文件名顺序执行 `apps/shortlink_server/sql/` 下的 SQL 脚本。
- 如果 MySQL 数据卷已经存在，初始化 SQL 不会重复执行。
- 开发环境用户名、密码和数据库名以 `compose.yaml` 为准。

构建并启动本地 Compose 环境：

```bash
cd /Users/hao/Code/haoHTTP
docker compose up -d --build
```

如果当前网络无法访问 Docker Hub，可以临时指定镜像源：

```bash
SHORTLINK_SERVER_BASE_IMAGE=docker.m.daocloud.io/library/ubuntu:22.04 \
SHORTLINK_NGINX_IMAGE=docker.m.daocloud.io/library/nginx:alpine \
SHORTLINK_MYSQL_IMAGE=docker.m.daocloud.io/library/mysql:8.0 \
SHORTLINK_REDIS_IMAGE=docker.m.daocloud.io/library/redis:7-alpine \
SHORTLINK_PROMETHEUS_IMAGE=docker.m.daocloud.io/prom/prometheus:v3.13.0 \
SHORTLINK_GRAFANA_IMAGE=docker.m.daocloud.io/grafana/grafana:12.4.3-security-02 \
docker compose up -d --build
```

如果当前网络无法访问默认 muduo 源码包地址，可以临时指定一个可访问的 muduo tarball 地址：

```bash
SHORTLINK_MUDUO_TARBALL_URL=https://example.com/path/to/muduo.tar.gz \
docker compose build shortlink_server
```

如果只想启动 MySQL 和 Redis 依赖，供 `haoHTTP` VM 中手工运行的 `shortlink_server` 使用：

```bash
docker compose up -d mysql redis
```

查看状态：

```bash
docker compose ps
```

查看日志：

```bash
docker compose logs mysql
docker compose logs redis
docker compose logs shortlink_server
docker compose logs nginx
docker compose logs prometheus
docker compose logs grafana
```

通过 Nginx 验证服务健康检查：

```bash
curl -i http://127.0.0.1:8080/api/health
```

通过 Nginx 验证创建短链接：

```bash
curl -i -X POST http://127.0.0.1:8080/api/short-links \
  -H 'Content-Type: application/json' \
  -d '{"url":"https://example.com/compose-container"}'
```

通过 Nginx 验证短码跳转：

```bash
curl -i http://127.0.0.1:8080/s/000001
```

如果需要绕过 Nginx 直连业务服务调试：

```bash
curl -i http://127.0.0.1:18080/api/health
```

直连服务查看 Prometheus 文本指标：

```bash
curl -i http://127.0.0.1:18080/metrics
```

Nginx 默认不转发 `/metrics`。以下请求应返回 Nginx 自身的 `404`：

```bash
curl -i http://127.0.0.1:8080/metrics
```

打开 Prometheus：

```text
http://127.0.0.1:9090
```

`Status -> Target health` 中的 `hao-shortlink` target 应为 `UP`。

打开 Grafana：

```text
http://127.0.0.1:3000
```

首次初始化的本地默认登录为 `admin` / `admin`，可以通过 `GRAFANA_ADMIN_USER` 和
`GRAFANA_ADMIN_PASSWORD` 环境变量覆盖。已有 `hao_shortlink_grafana_data` 数据卷时，管理员
凭据继续使用卷内保存的值。进入 `Dashboards -> HaoShortLink` 后可打开 `HaoShortLink Overview`。

执行完整监控冒烟验证：

```bash
bash tests/scripts/monitoring_smoke_test.sh
```

脚本会启动 Compose、制造创建和跳转流量、验证 Prometheus target 与业务指标、验证 Grafana
数据源和 dashboard、确认 Nginx 不暴露 `/metrics`，并重建 Prometheus / Grafana 容器检查历史数据和 dashboard。

验证 MySQL 表结构：

```bash
docker exec hao-shortlink-mysql \
  mysql -u hao_shortlink -phao_shortlink hao_shortlink \
  -e 'SHOW TABLES;'
```

验证 Redis：

```bash
docker exec hao-shortlink-redis redis-cli ping
```

在 `haoHTTP` VM 中验证依赖端口可访问：

```bash
ssh haoHTTP@orb
mysql -h docker.orb.internal -P 13306 -u hao_shortlink -phao_shortlink hao_shortlink \
  -e 'SHOW TABLES;'
redis-cli -h docker.orb.internal -p 16379 ping
```

在 `haoHTTP` VM 中验证 Redis 地址路径：

```bash
getent ahosts docker.orb.internal
HAOHTTP_REDIS_HOST=<resolved-ipv6-address> \
  HAOHTTP_REDIS_SEGMENT_REQUESTS=20 \
  bash tests/scripts/redis_hiredis_segment_diagnostic.sh
```

说明：

- `<resolved-ipv6-address>` 是当前本机 OrbStack 网络解析出的 IPv6 地址，例如 `fd07:...`。
- 该地址属于本地开发环境，不应写死到公开默认配置或生产配置。
- 如果 `docker.orb.internal` / IPv4 路径出现约 0.2s Redis 命令等待，优先用该诊断确认是否为本地网络路径问题。
- v1.4.5 已验证当前环境使用 IPv6 字面地址后，HTTP Redis hit 可恢复到约 0.0003s；该结论只适用于当前本地 OrbStack VM 验证路径。

停止依赖：

```bash
docker compose down
```

清空依赖数据并停止：

```bash
docker compose down -v
```

注意：

- `docker compose` 使用 OrbStack 自带 Docker，不在 `haoHTTP` VM 内执行。
- `docker compose down` 会停止并删除容器，但默认保留 MySQL、Redis、Prometheus 和 Grafana 数据卷。
- `docker compose down -v` 会删除全部数据卷，MySQL 数据、Redis 数据和监控历史也会被清空。

## 服务启动

服务二进制由 Linux VM 中的构建目录产生。日常使用挂载目录构建时，默认路径为：

```text
/tmp/haoHTTP-build/shortlink_server
```

### 内存存储模式

内存模式适合快速验证 HTTP 服务和短链 API，不依赖 MySQL 或 Redis。进程重启后短链数据会丢失。

配置要点：

```text
storage.type=memory
redis.enabled=false
```

启动命令：

```bash
ssh haoHTTP@orb
cd /Users/hao/Code/haoHTTP
/tmp/haoHTTP-build/shortlink_server apps/shortlink_server/config/server.conf.example
```

### MySQL 持久化模式

MySQL 模式用于验证短链数据持久化。MySQL 是短链数据的事实来源。

配置要点：

```text
storage.type=mysql
mysql.host=tcp://127.0.0.1:3306
mysql.user=hao_shortlink
mysql.password=change_me
mysql.database=hao_shortlink
mysql.pool_size=4
redis.enabled=false
```

数据库初始化脚本：

```text
apps/shortlink_server/sql/
```

初始化表结构示例：

```bash
ssh haoHTTP@orb
mysql -u hao_shortlink -p hao_shortlink < /Users/hao/Code/haoHTTP/apps/shortlink_server/sql/001_create_short_links.sql
mysql -u hao_shortlink -p hao_shortlink < /Users/hao/Code/haoHTTP/apps/shortlink_server/sql/002_make_short_link_code_case_sensitive.sql
```

启动命令：

```bash
ssh haoHTTP@orb
cd /Users/hao/Code/haoHTTP
/tmp/haoHTTP-build/shortlink_server /path/to/local-server.conf
```

### MySQL + Redis 查询缓存模式

Redis 只作为短码跳转查询缓存，不承担短链事实存储。Redis 不可用时，服务应继续回源 MySQL。

配置要点：

```text
storage.type=mysql
redis.enabled=true
redis.host=127.0.0.1
redis.port=6379
redis.database=0
redis.ttl_seconds=3600
redis.key_prefix=shortlink:
```

启动命令与 MySQL 模式相同，区别只在配置文件。

使用 Docker Compose 依赖时，可以直接使用：

```text
apps/shortlink_server/config/server.compose.conf.example
```

启动命令：

```bash
ssh haoHTTP@orb
cd /Users/hao/Code/haoHTTP
/tmp/haoHTTP-build/shortlink_server apps/shortlink_server/config/server.compose.conf.example
```

## 手工接口验证

以下命令在 Linux VM 中执行，服务默认监听 `8080` 端口。

健康检查：

```bash
curl -i http://127.0.0.1:8080/api/health
```

创建短链接：

```bash
curl -i \
  -H 'Content-Type: application/json' \
  -d '{"url":"https://example.com/very/long/path"}' \
  http://127.0.0.1:8080/api/short-links
```

短码跳转：

```bash
curl -i http://127.0.0.1:8080/s/000001
```

不存在短码：

```bash
curl -i http://127.0.0.1:8080/s/notfound
```

非法 URL：

```bash
curl -i \
  -H 'Content-Type: application/json' \
  -d '{"url":"ftp://example.com/file"}' \
  http://127.0.0.1:8080/api/short-links
```

## 常见故障排查

### 服务启动后接口无法访问

检查项：

- 服务是否在 Linux VM 中启动，而不是 Mac 宿主机。
- 配置中的 `server.port` 是否为预期端口。
- 端口是否被已有进程占用。
- curl 是否请求 `127.0.0.1` 和正确端口。

### MySQL 模式创建短链失败

检查项：

- `storage.type` 是否为 `mysql`。
- MySQL 服务是否启动。
- `mysql.host`、`mysql.user`、`mysql.password`、`mysql.database` 是否和本地环境一致。
- 是否已经执行 `001_create_short_links.sql`。
- 用户是否有 `SELECT`、`INSERT`、`UPDATE` 权限。

### Redis 开启后跳转变慢或日志报错

检查项：

- Redis 服务是否启动。
- `redis.host`、`redis.port`、`redis.database` 是否正确。
- Redis 只是查询缓存；如果 MySQL 可用，Redis 连接失败不应导致已有短链返回 404。
- 如果使用 Docker Compose 依赖，确认服务配置中的 `redis.host` 是 `docker.orb.internal`，`redis.port` 是 `16379`。
- 如果功能正常但 Redis hit / missing-code 延迟稳定约 0.2s，使用 `tests/scripts/redis_hiredis_segment_diagnostic.sh`
  对比 `docker.orb.internal`、显式 IPv4 和当前解析出的 IPv6 字面地址；当前 OrbStack VM 环境中已观察到 IPv6 路径可恢复到亚毫秒级。

### Docker Compose 依赖无法启动

检查项：

- OrbStack Docker 是否可用。
- 是否在仓库根目录通过 OrbStack Docker 执行 `docker compose up -d`。
- `13306` 或 `16379` 是否被其他进程占用。
- 第一次启动 MySQL 时是否需要等待初始化完成。
- 如需完全重建空数据库，确认可以接受删除数据后再执行 `docker compose down -v`。

### 短链重启后丢失

检查项：

- 如果使用 `storage.type=memory`，这是预期行为。
- 需要重启后仍可跳转时，使用 `storage.type=mysql`。

## 构建验证

日常挂载验证命令：

```bash
ssh haoHTTP@orb
cmake -S /Users/hao/Code/haoHTTP -B /tmp/haoHTTP-build
cmake --build /tmp/haoHTTP-build -j2
```

干净克隆验证命令示例：

```bash
ssh haoHTTP@orb
git clone --branch refactor/hao-shortlink-cleanup https://github.com/BeginAgainR/HaoShortLink.git /tmp/haoHTTP-clean-check
cmake -S /tmp/haoHTTP-clean-check -B /tmp/haoHTTP-clean-check-build
cmake --build /tmp/haoHTTP-clean-check-build -j2
```

最近一次 v1.1 干净克隆验证结果：

```text
[100%] Built target shortlink_server
```

最近一次 v1.1 输出文件：

```text
/tmp/haoHTTP-clean-v1.1-final-build/shortlink_server
```

## v1.0 验证记录

v1.0 实现过程中，每个可运行节点都应在 Linux VM 中验证，不在 Mac 宿主机上构建或运行。

### 服务入口和健康检查

状态：已完成。

计划验证内容：

- `cmake -S /Users/hao/Code/haoHTTP -B /tmp/haoHTTP-build`
- `cmake --build /tmp/haoHTTP-build -j2`
- 启动 `/tmp/haoHTTP-build/shortlink_server`
- 使用 curl 请求 `GET /api/health`
- 确认响应状态码为 200，响应体包含 `{"status":"ok"}`

最近一次结果：

```text
[100%] Built target shortlink_server
HTTP/1.1 200 OK
{"status":"ok"}
```

### 创建短链接

状态：已完成。

计划验证内容：

- 使用 curl 请求 `POST /api/short-links`
- 请求体包含 `{"url":"https://example.com/very/long/path"}`
- 确认响应状态码为 201
- 确认响应体包含 `code`、`short_url` 和 `original_url`
- 使用非法 JSON、缺失 `url` 和非法 URL 验证 400 JSON 错误

最近一次结果：

```text
HTTP/1.1 201 Created
{"code":"000001","short_url":"/s/000001","original_url":"https://example.com/very/long/path"}

HTTP/1.1 400 URL must start with http:// or https://
{"error":{"code":"invalid_url","message":"URL must start with http:// or https://"}}

HTTP/1.1 400 Request body must contain a string url field
{"error":{"code":"invalid_request","message":"Request body must contain a string url field"}}
```

### 内存版短链核心

状态：已完成。

最近一次结果：

```text
-- ShortLink Server sources: /Users/hao/Code/haoHTTP/apps/shortlink_server/src/ShortLinkService.cpp
[100%] Built target shortlink_server
```

说明：

- 当前只验证内存版核心参与编译。
- 创建短链接和短码跳转 HTTP 接口已接入。

### 短码跳转

状态：已完成。

计划验证内容：

- 使用创建接口返回的 `short_url` 请求跳转路径
- 确认响应状态码为 302
- 确认 `Location` 指向原始 URL
- 使用不存在的短码验证 404 JSON 错误

最近一次结果：

```text
HTTP/1.1 302 Found
Location: https://example.com/very/long/path

HTTP/1.1 404 Short link not found
{"error":{"code":"short_link_not_found","message":"Short link not found"}}
```

### v1.0 收口验证

状态：已完成。

验证内容：

- 在 Linux VM 中进行最终构建验证。
- 手工跑通健康检查、创建短链接、短码跳转。
- 确认文档只声明 V1 内存版能力。
- 确认 README 和 `docs/` 未声明 MySQL、Redis、Docker Compose、Nginx 等未实现能力。
- 确认仓库内没有生成或提交 `build/` 目录。

日常挂载目录最近一次结果：

```text
[100%] Built target shortlink_server

GET /api/health
HTTP/1.1 200 OK
{"status":"ok"}

POST /api/short-links
HTTP/1.1 201 Created
{"code":"000001","short_url":"/s/000001","original_url":"https://example.com/very/long/path"}

GET /s/000001
HTTP/1.1 302 Found
Location: https://example.com/very/long/path

POST /api/short-links with invalid JSON
HTTP/1.1 400 Request body must contain a string url field
{"error":{"code":"invalid_request","message":"Request body must contain a string url field"}}

POST /api/short-links with invalid URL
HTTP/1.1 400 URL must start with http:// or https://
{"error":{"code":"invalid_url","message":"URL must start with http:// or https://"}}

GET /s/notfound
HTTP/1.1 404 Short link not found
{"error":{"code":"short_link_not_found","message":"Short link not found"}}
```

远端干净克隆验证：

```text
分支：refactor/hao-shortlink-cleanup
提交：d06d7c1
路径：/tmp/haoHTTP-clean-v1.0-remote
构建目录：/tmp/haoHTTP-clean-v1.0-remote-build
结果：[100%] Built target shortlink_server

GET /api/health
HTTP/1.1 200 OK
{"status":"ok"}

POST /api/short-links
HTTP/1.1 201 Created
{"code":"000001","short_url":"/s/000001","original_url":"https://example.com/clean-v1"}

GET /s/000001
HTTP/1.1 302 Found
Location: https://example.com/clean-v1

GET /s/notfound
HTTP/1.1 404 Short link not found
{"error":{"code":"short_link_not_found","message":"Short link not found"}}

POST /api/short-links with invalid URL
HTTP/1.1 400 URL must start with http:// or https://
{"error":{"code":"invalid_url","message":"URL must start with http:// or https://"}}
```

## v1.1 验证计划

当前状态：已完成。

v1.1 已引入 MySQL 持久化和 Redis 查询缓存。MySQL 是事实来源，Redis 只作为短码跳转查询缓存。

### 存储层抽象验证

状态：已完成。

验证内容：

- `ShortLinkService` 依赖 repository 接口。
- 当前默认 repository 仍为内存实现。
- 不改变 V1 对外 API 和响应行为。

最近一次结果：

```text
构建目录：/tmp/haoHTTP-build
结果：[100%] Built target shortlink_server

GET /api/health
HTTP/1.1 200 OK
{"status":"ok"}

POST /api/short-links
HTTP/1.1 201 Created
{"code":"000001","short_url":"/s/000001","original_url":"https://example.com/repository"}

GET /s/000001
HTTP/1.1 302 Found
Location: https://example.com/repository

GET /s/notfound
HTTP/1.1 404 Short link not found
{"error":{"code":"short_link_not_found","message":"Short link not found"}}

POST /api/short-links with invalid URL
HTTP/1.1 400 URL must start with http:// or https://
{"error":{"code":"invalid_url","message":"URL must start with http:// or https://"}}
```

已完成验证项：

- MySQL 表结构脚本可以在 Linux VM 中执行。
- 创建短链接后，MySQL 中存在对应记录。
- 服务重启后，已创建短链接仍可跳转。
- Redis 未命中时可以回源 MySQL。
- Redis 命中时可以直接返回跳转。
- Redis 不可用时不应把已有短链误判为不存在。
- 远端干净克隆验证可以构建并跑通 v1.1 核心接口。

### MySQL 表结构和创建持久化验证

状态：已完成。

已完成内容：

- 新增 `apps/shortlink_server/sql/001_create_short_links.sql`。
- 新增 MySQL repository 创建路径。
- `storage.type=mysql` 时初始化 MySQL 连接池并使用 MySQL repository。
- 创建短链接时写入 MySQL，通过自增 id 生成 Base62 短码并回写 `code`。

当前 VM 已安装：

- `mysql-server`
- `mysql-client`
- MySQL 开发库和 MySQL Connector/C++
- `redis-server`
- `redis-tools`
- `libhiredis-dev`

当前 VM 已创建：

- 数据库：`hao_shortlink`
- 用户：`hao_shortlink`
- 表：`short_links`

最近一次已完成验证：

```text
构建目录：/tmp/haoHTTP-build
结果：[100%] Built target shortlink_server

storage.type=memory 回归：
GET /api/health -> HTTP/1.1 200 OK
POST /api/short-links -> HTTP/1.1 201 Created
GET /s/000001 -> HTTP/1.1 302 Found

storage.type=mysql 验证：
GET /api/health -> HTTP/1.1 200 OK
POST /api/short-links -> HTTP/1.1 201 Created
GET /s/000001 -> HTTP/1.1 302 Found

MySQL 查询结果：
1    000001    https://example.com/mysql-created
```

说明：

- MySQL 创建持久化路径已验证。
- 服务重启后从 MySQL 跳转已验证。

### MySQL 跳转查询验证

状态：已完成。

验证内容：

- `storage.type=mysql` 时，短码跳转从 MySQL 查询原始 URL。
- 服务重启后，进程内状态清空，已创建短链仍可跳转。
- 不存在的短码仍返回 404。

最近一次已完成验证：

```text
构建目录：/tmp/haoHTTP-build
结果：[100%] Built target shortlink_server

GET /api/health -> HTTP/1.1 200 OK
POST /api/short-links -> HTTP/1.1 201 Created
创建短码：000001

MySQL 查询结果：
1    000001    https://example.com/mysql-restart

重启服务后：
GET /s/000001 -> HTTP/1.1 302 Found
Location: https://example.com/mysql-restart

GET /s/notfound -> HTTP/1.1 404 Short link not found
```

### Redis 查询缓存验证

状态：已完成。

已完成内容：

- 新增 Redis 查询缓存组件。
- 新增 Redis 缓存包装 repository。
- `storage.type=mysql` 且 `redis.enabled=true` 时，短码跳转先查 Redis。
- Redis 未命中时回源 MySQL，MySQL 命中后回填 Redis。
- Redis 不可用时继续回源 MySQL，不直接改变为 404。
- `storage.type=memory` 时忽略 Redis 开关。

最近一次已完成验证：

```text
构建目录：/tmp/haoHTTP-build
结果：[100%] Built target shortlink_server

GET /api/health -> HTTP/1.1 200 OK
POST /api/short-links -> HTTP/1.1 201 Created
创建短码：000001

第一次跳转前 Redis：
GET shortlink:000001 -> <nil>

第一次跳转：
GET /s/000001 -> HTTP/1.1 302 Found
Location: https://example.com/redis-cache

第一次跳转后 Redis：
GET shortlink:000001 -> https://example.com/redis-cache

MySQL 临时停止后验证 Redis 命中：
GET /s/000001 -> HTTP/1.1 302 Found
Location: https://example.com/redis-cache

Redis 端口不可用时验证 MySQL 回退：
GET /s/000001 -> HTTP/1.1 302 Found
Location: https://example.com/redis-cache
```

### v1.1 最终收口验证

状态：已完成。

验证内容：

- Linux VM 挂载目录构建。
- MySQL + Redis 模式健康检查。
- 创建短链接后写入 MySQL。
- Redis 首次未命中后回源 MySQL 并回填 Redis。
- 短码跳转返回 302。
- 不存在短码返回 404。
- 非法 URL 返回 400。

最近一次已完成验证：

```text
构建目录：/tmp/haoHTTP-build
结果：[100%] Built target shortlink_server

GET /api/health -> HTTP/1.1 200 OK
POST /api/short-links -> HTTP/1.1 201 Created
创建短码：000001

MySQL 查询结果：
1    000001    https://example.com/v1.1-final

第一次跳转前 Redis：
GET shortlink:000001 -> <nil>

GET /s/000001 -> HTTP/1.1 302 Found
Location: https://example.com/v1.1-final

第一次跳转后 Redis：
GET shortlink:000001 -> https://example.com/v1.1-final

GET /s/notfound -> HTTP/1.1 404 Short link not found
POST /api/short-links with invalid URL -> HTTP/1.1 400 URL must start with http:// or https://
```

远端干净克隆验证：

```text
分支：refactor/hao-shortlink-cleanup
提交：4a53281
路径：/tmp/haoHTTP-clean-v1.1-final
构建目录：/tmp/haoHTTP-clean-v1.1-final-build
结果：[100%] Built target shortlink_server
```

## v1.2 验证记录

### MySQL / Redis Docker Compose 依赖验证

状态：已完成。

验证日期：

```text
2026-07-07
```

验证内容：

- 使用 OrbStack Docker 启动 `compose.yaml` 中的 MySQL 和 Redis。
- `shortlink_server` 仍在 OrbStack Linux VM `haoHTTP` 中构建和运行。
- `haoHTTP` 通过 `docker.orb.internal` 连接 Compose 依赖。
- MySQL 首次初始化后存在 `short_links` 表。
- Redis 可以返回 `PONG`。
- 服务可以使用 `server.compose.conf.example` 跑通短链接口闭环。

最近一次结果：

```text
Docker Compose：
hao-shortlink-mysql -> Up (healthy), 0.0.0.0:13306->3306/tcp
hao-shortlink-redis -> Up (healthy), 0.0.0.0:16379->6379/tcp

MySQL 表结构：
SHOW TABLES -> short_links

Redis：
PING -> PONG

haoHTTP 连接 Compose 依赖：
mysql -h docker.orb.internal -P 13306 -> short_links
redis-cli -h docker.orb.internal -p 16379 ping -> PONG

构建：
[100%] Built target shortlink_server

GET /api/health：
{"status":"ok"}

POST /api/short-links：
HTTP/1.1 201 Created
{"code":"000001","short_url":"/s/000001","original_url":"https://example.com/compose"}

GET /s/000001：
HTTP/1.1 302 Found
Location: https://example.com/compose

Redis 回填：
GET shortlink:000001 -> https://example.com/compose

MySQL 查询：
1    000001    https://example.com/compose

GET /s/notfound：
HTTP/1.1 404 Short link not found

POST /api/short-links with invalid URL：
HTTP/1.1 400 URL must start with http:// or https://
```

说明：

- 默认镜像仍为 `mysql:8.0` 和 `redis:7-alpine`。
- 本次验证时 Docker Hub 访问超时，临时使用 `SHORTLINK_MYSQL_IMAGE` 和 `SHORTLINK_REDIS_IMAGE` 指定镜像源完成拉取。
- `haoHTTP` VM 内不需要安装 Docker；Docker Compose 依赖由 OrbStack Docker 管理。

### shortlink_server Docker Compose 服务容器验证

状态：已完成。

验证日期：

```text
2026-07-07
```

验证内容：

- 使用 `Dockerfile` 构建 `hao-shortlink-server:dev`。
- Dockerfile 固定 muduo 源码版本，只构建 `muduo_base` 和 `muduo_net`。
- `shortlink_server` 容器使用 `server.container.conf.example`。
- `shortlink_server` 容器通过 Compose 服务名 `mysql`、`redis` 连接依赖。
- 从 `127.0.0.1:18080` 访问容器服务，跑通健康检查、创建短链接、短码跳转、Redis 回填和 MySQL 查询。

最近一次结果：

```text
构建镜像：
hao-shortlink-server:dev -> Built
[100%] Built target shortlink_server

Docker Compose：
hao-shortlink-mysql -> Up (healthy), 0.0.0.0:13306->3306/tcp
hao-shortlink-redis -> Up (healthy), 0.0.0.0:16379->6379/tcp
hao-shortlink-server -> Up (healthy), 0.0.0.0:18080->8080/tcp

GET /api/health：
HTTP/1.1 200 OK
{"status":"ok"}

POST /api/short-links：
HTTP/1.1 201 Created
{"code":"000002","short_url":"/s/000002","original_url":"https://example.com/compose-container"}

GET /s/000002：
HTTP/1.1 302 Found
Location: https://example.com/compose-container

Redis 回填：
GET shortlink:000002 -> https://example.com/compose-container

MySQL 查询：
2    000002    https://example.com/compose-container

GET /s/notfound：
HTTP/1.1 404 Short link not found

POST /api/short-links with invalid URL：
HTTP/1.1 400 URL must start with http:// or https://
```

### Nginx Docker Compose 反向代理验证

状态：已完成。

验证日期：

```text
2026-07-07
```

验证内容：

- 使用 `deploy/nginx/default.conf` 作为 Nginx 配置。
- Nginx 容器监听内部 80 端口，对外发布 `127.0.0.1:8080`。
- Nginx 通过 Compose 服务名 `shortlink_server:8080` 转发请求。
- `/api/` 和 `/s/` 路径经 Nginx 转发到 `shortlink_server`。
- `/` 路径由 Nginx 返回 404 JSON。
- 经 Nginx 跑通健康检查、创建短链接、短码跳转、Redis 回填和 MySQL 查询。

最近一次结果：

```text
Nginx 配置检查：
nginx: configuration file /etc/nginx/nginx.conf test is successful

Docker Compose：
hao-shortlink-nginx -> Up (healthy), 0.0.0.0:8080->80/tcp
hao-shortlink-server -> Up (healthy), 0.0.0.0:18080->8080/tcp
hao-shortlink-mysql -> Up (healthy), 0.0.0.0:13306->3306/tcp
hao-shortlink-redis -> Up (healthy), 0.0.0.0:16379->6379/tcp

GET /api/health via Nginx：
HTTP/1.1 200 OK
Server: nginx/1.31.2
{"status":"ok"}

POST /api/short-links via Nginx：
HTTP/1.1 201 Created
Server: nginx/1.31.2
{"code":"000003","short_url":"/s/000003","original_url":"https://example.com/nginx-proxy"}

GET /s/000003 via Nginx：
HTTP/1.1 302 Found
Server: nginx/1.31.2
Location: https://example.com/nginx-proxy

Redis 回填：
GET shortlink:000003 -> https://example.com/nginx-proxy

MySQL 查询：
3    000003    https://example.com/nginx-proxy

GET /s/notfound via Nginx：
HTTP/1.1 404 Short link not found

POST /api/short-links with invalid URL via Nginx：
HTTP/1.1 400 URL must start with http:// or https://

GET / via Nginx：
HTTP/1.1 404 Not Found
{"error":{"code":"not_found","message":"Not Found"}}
```

### v1.2 本地干净克隆 Compose 验证

状态：已完成。

验证日期：

```text
2026-07-07
```

验证内容：

- 从已提交的本地仓库克隆到 `/tmp/haoHTTP-clean-v1.2-final`。
- 克隆提交为 `ce3f605`。
- 使用临时 Compose override 修改容器名和宿主机端口，避免和当前开发栈冲突。
- 从干净源码构建 `hao-shortlink-server:v1.2-clean`。
- 创建独立 Docker network 和独立 MySQL/Redis 数据卷。
- 经 Nginx 跑通健康检查、创建短链接、短码跳转、Redis 回填、MySQL 查询、404 和非法 URL。
- 验证完成后停止并删除临时验证栈和临时数据卷。

临时端口：

```text
Nginx: 28080 -> 80
shortlink_server: 28081 -> 8080
MySQL: 23306 -> 3306
Redis: 26379 -> 6379
```

最近一次结果：

```text
干净克隆：
/tmp/haoHTTP-clean-v1.2-final
提交：ce3f605 chore: add nginx reverse proxy

Docker Compose：
hao-shortlink-clean-nginx -> Up (healthy), 0.0.0.0:28080->80/tcp
hao-shortlink-clean-server -> Up (healthy), 0.0.0.0:28081->8080/tcp
hao-shortlink-clean-mysql -> Up (healthy), 0.0.0.0:23306->3306/tcp
hao-shortlink-clean-redis -> Up (healthy), 0.0.0.0:26379->6379/tcp

GET /api/health via clean Nginx：
HTTP/1.1 200 OK
Server: nginx/1.31.2
{"status":"ok"}

POST /api/short-links via clean Nginx：
HTTP/1.1 201 Created
{"code":"000001","short_url":"/s/000001","original_url":"https://example.com/v1.2-clean"}

GET /s/000001 via clean Nginx：
HTTP/1.1 302 Found
Location: https://example.com/v1.2-clean

Redis 回填：
GET shortlink:000001 -> https://example.com/v1.2-clean

MySQL 查询：
1    000001    https://example.com/v1.2-clean

GET /s/notfound via clean Nginx：
HTTP/1.1 404 Short link not found

POST /api/short-links with invalid URL via clean Nginx：
HTTP/1.1 400 URL must start with http:// or https://

GET / via clean Nginx：
HTTP/1.1 404 Not Found
{"error":{"code":"not_found","message":"Not Found"}}

GET /api/health direct service debug port：
HTTP/1.1 200 OK
{"status":"ok"}
```

边界说明：

- v1.2 的 Compose 配置面向本地开发和验证。
- 当前保留 `18080` 作为 `shortlink_server` 直连调试入口。
- 生产化部署应只对外暴露 Nginx 的 80/443，`shortlink_server`、MySQL 和 Redis 应仅在内部网络可见。
- HTTPS/TLS 终止后续由 Nginx 配置承接，当前尚未实现。

## 当前文档任务验证

文档任务不需要运行构建命令。验证重点是：

- Markdown 文件是否存在。
- README 文档入口是否正确。
- 未实现能力是否明确标注。
- 是否误改 C++ 或 CMake 文件。
