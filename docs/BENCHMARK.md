# 压测计划

状态：v1.4 进行中
当前实现：v1.4.0 已完成压测与稳定性阶段定界；v1.4.1 已新增压测脚本入口和结果记录格式；v1.4.2 已完成第一轮 curl fallback 多模式基线；hey 压测基线尚未执行

## 说明

本文档用于记录 HaoShortLink v1.4 的压测目标、工具、环境和结果。当前短链接核心接口、
MySQL 持久化、Redis 查询缓存、本地 Compose 编排和第一版测试 / CI 已完成；v1.4 将在这些基础上建立可重复的性能与稳定性基线。

## v1.4 目标

- 建立可重复执行的压测入口。
- 记录健康检查、创建短链和短码跳转的基线表现。
- 对比内存存储、MySQL 持久化、MySQL + Redis 查询缓存三种模式。
- 记录 QPS、平均延迟、P95、P99 和错误率。
- 通过压测结果评估 Redis 连接复用、MySQL 连接池参数和 worker 线程数量关系。
- 覆盖关键异常场景，确认服务错误响应稳定。

## v1.4 非目标

- 不声明生产最大承载能力。
- 不接入 Prometheus、Grafana 或完整指标系统。
- 不在没有压测证据时重写连接池。
- 不实现用户系统、访问统计、消息队列或后台管理。
- 不把一次本地压测结果包装成通用性能承诺。

## 计划压测环境

初始压测环境以本地可复现为主：

- 运行环境：OrbStack Linux VM `haoHTTP`。
- 项目路径：`/Users/hao/Code/haoHTTP`。
- 构建目录：`/tmp/haoHTTP-build`。
- 依赖服务：OrbStack Docker Compose 中的 MySQL、Redis 和 Nginx。
- 服务入口：优先测试 `shortlink_server` 直连端口，必要时补充 Nginx 入口对比。

每次记录结果时，需要写清楚：

- git commit。
- 配置文件。
- `server.thread_num`。
- `mysql.pool_size`。
- `redis.enabled`。
- 压测工具和版本。
- 并发数、请求总数或持续时间。

## 当前压测入口

当前压测脚本：

```bash
bash tests/scripts/benchmark_shortlink.sh
```

脚本默认：

- 使用 `/tmp/haoHTTP-build/shortlink_server`。
- 自动选择压测工具；优先使用 `hey`，未安装时使用 `curl` 并发循环。
- 使用内存存储模式。
- 并发数为 `16`。
- 请求数为 `1000`。
- 自动启动临时 `shortlink_server`，结束后清理进程和临时目录。
- 输出可复制到本文档的 Markdown 表格行。

常用示例：

```bash
HAOHTTP_BENCH_SCENARIO=health \
HAOHTTP_BENCH_MODE=memory \
HAOHTTP_BENCH_REQUESTS=1000 \
HAOHTTP_BENCH_CONCURRENCY=16 \
bash tests/scripts/benchmark_shortlink.sh
```

```bash
HAOHTTP_BENCH_SCENARIO=redirect \
HAOHTTP_BENCH_MODE=mysql-redis \
HAOHTTP_BENCH_REQUESTS=1000 \
HAOHTTP_BENCH_CONCURRENCY=16 \
bash tests/scripts/benchmark_shortlink.sh
```

```bash
HAOHTTP_BENCH_SCENARIO=redirect-cache-hit \
HAOHTTP_BENCH_MODE=mysql-redis \
HAOHTTP_BENCH_REQUESTS=1000 \
HAOHTTP_BENCH_CONCURRENCY=16 \
bash tests/scripts/benchmark_shortlink.sh
```

支持场景：

- `health`
- `create`
- `redirect`
- `redirect-cache-hit`
- `redirect-cache-miss`
- `invalid-url`
- `missing-code`
- `all`

支持模式：

- `memory`
- `mysql`
- `mysql-redis`

可配置变量：

- `HAOHTTP_BUILD_DIR`
- `HAOHTTP_SERVER_BIN`
- `HAOHTTP_BENCH_TOOL`
- `HAOHTTP_HEY_BIN`
- `HAOHTTP_BENCH_PORT`
- `HAOHTTP_BENCH_THREAD_NUM`
- `HAOHTTP_BENCH_MYSQL_POOL_SIZE`
- `HAOHTTP_BENCH_REQUESTS`
- `HAOHTTP_BENCH_CONCURRENCY`
- `HAOHTTP_BENCH_SCENARIO`
- `HAOHTTP_BENCH_MODE`
- `HAOHTTP_MYSQL_HOST`
- `HAOHTTP_MYSQL_PORT`
- `HAOHTTP_MYSQL_USER`
- `HAOHTTP_MYSQL_PASSWORD`
- `HAOHTTP_MYSQL_DATABASE`
- `HAOHTTP_REDIS_HOST`
- `HAOHTTP_REDIS_PORT`
- `HAOHTTP_REDIS_KEY_PREFIX`

当前验证：

- 已完成脚本 Bash 语法检查。
- 已完成脚本 `--help` 输出验证。
- 当前本地和 `haoHTTP` VM 尚未安装 `hey`；脚本会自动回退到 `curl` 模式。
- 已使用 `curl` 模式完成小请求量脚本链路验证；该验证只确认脚本可运行，不作为性能基线记录。
- `redirect-cache-hit` 会预热 Redis 后压测跳转。
- `redirect-cache-miss` 会删除 Redis key，并强制只执行 1 个请求，用于观察单次未命中回源成本。

## 计划压测场景

### 基础 HTTP

- `GET /api/health`
- 目标：观察框架和服务入口基础开销。
- 存储依赖：无。

### 创建短链

- `POST /api/short-links`
- 目标：观察 JSON 请求处理、URL 校验、短码生成和写入路径。
- 测试模式：
  - `storage.type=memory`
  - `storage.type=mysql`
  - `storage.type=mysql` 且 `redis.enabled=true`

### 短码跳转

- `GET /s/{code}`
- 目标：观察查询路径和 302 响应开销。
- 测试模式：
  - 内存命中。
  - MySQL 命中。
  - Redis 未命中后回源 MySQL。
  - Redis 命中。

### 错误响应

- 非法 URL 创建请求。
- 不存在短码跳转。
- 目标：观察高并发错误响应是否稳定。

## 计划异常场景

- Redis 不可用时，已有短链应继续回源 MySQL。
- MySQL 不可用时，创建短链和 MySQL 未缓存跳转应稳定返回错误，不应导致服务崩溃。
- 非法请求高并发时，应稳定返回 JSON 错误。
- 不存在短码高并发时，应稳定返回 `404 Not Found` JSON。

## 结果记录格式

压测结果使用表格记录，示例格式如下。当前没有实测数据，先保留为空模板。

| 场景 | 模式 | 并发 | 总请求/时长 | QPS | 平均延迟 | P95 | P99 | 错误率 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |

## v1.4.2 第一轮基线

本轮只作为本地 curl fallback 基线，用于观察相对差异和暴露问题，不声明生产最大承载能力。

环境：

- 环境：OrbStack Linux VM `haoHTTP`
- commit：`8e3b494`
- 构建目录：`/tmp/haoHTTP-build`
- 压测工具：脚本 `curl` fallback
- 请求数：`500`
- 并发数：`16`
- `server.thread_num=4`
- `mysql.pool_size=4`
- 依赖：OrbStack Docker Compose 中的 MySQL 和 Redis，均为 healthy

结果：

| 场景 | 模式 | 并发 | 总请求/时长 | QPS | 平均延迟 | P95 | P99 | 错误率 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| GET /api/health | memory | 16 | 500 requests | 2439.02 | 0.000379s | 0.001616s | 0.002072s | 0.00% | tool curl; expected 200; commit 8e3b494 |
| POST /api/short-links | memory | 16 | 500 requests | 2659.57 | 0.000313s | 0.001456s | 0.001971s | 0.00% | tool curl; expected 201; commit 8e3b494 |
| GET /s/{code} | memory | 16 | 500 requests | 2304.15 | 0.000493s | 0.001997s | 0.003623s | 0.00% | tool curl; expected 302; commit 8e3b494 |
| POST invalid URL | memory | 16 | 500 requests | 2717.39 | 0.000383s | 0.001749s | 0.002360s | 0.00% | tool curl; expected 400; commit 8e3b494 |
| GET missing short code | memory | 16 | 500 requests | 2450.98 | 0.000376s | 0.001677s | 0.002471s | 0.00% | tool curl; expected 404; commit 8e3b494 |
| GET /api/health | mysql | 16 | 500 requests | 2732.24 | 0.000452s | 0.001859s | 0.002576s | 0.00% | tool curl; expected 200; commit 8e3b494 |
| POST /api/short-links | mysql | 16 | 500 requests | 1091.70 | 0.011425s | 0.020525s | 0.024454s | 41.40% | tool curl; expected 201; commit 8e3b494 |
| GET /s/{code} | mysql | 16 | 500 requests | 2475.25 | 0.001190s | 0.002493s | 0.003450s | 0.00% | tool curl; expected 302; commit 8e3b494 |
| POST invalid URL | mysql | 16 | 500 requests | 2840.91 | 0.000348s | 0.001646s | 0.002206s | 0.00% | tool curl; expected 400; commit 8e3b494 |
| GET missing short code | mysql | 16 | 500 requests | 2463.05 | 0.001118s | 0.002581s | 0.003260s | 0.00% | tool curl; expected 404; commit 8e3b494 |
| GET /api/health | mysql-redis | 16 | 500 requests | 2762.43 | 0.000476s | 0.001912s | 0.002702s | 0.00% | tool curl; expected 200; commit 8e3b494 |
| POST /api/short-links | mysql-redis | 16 | 500 requests | 1118.57 | 0.010933s | 0.018237s | 0.026043s | 40.40% | tool curl; expected 201; commit 8e3b494 |
| GET /s/{code} Redis miss | mysql-redis | 16 | 1 requests | 2.26 | 0.413934s | 0.413934s | 0.413934s | 0.00% | tool curl; expected 302; commit 8e3b494 |
| GET /s/{code} Redis hit | mysql-redis | 16 | 500 requests | 19.20 | 0.812246s | 0.845628s | 0.848110s | 0.00% | tool curl; expected 302; commit 8e3b494 |
| POST invalid URL | mysql-redis | 16 | 500 requests | 2564.10 | 0.000435s | 0.001835s | 0.002449s | 0.00% | tool curl; expected 400; commit 8e3b494 |
| GET missing short code | mysql-redis | 16 | 500 requests | 19.18 | 0.815036s | 0.833494s | 0.838910s | 0.00% | tool curl; expected 404; commit 8e3b494 |

初步观察：

- 内存模式五个场景错误率均为 0，QPS 在本轮 curl fallback 下约为 2300-2700。
- MySQL 模式的跳转和不存在短码查询错误率为 0，但创建短链在并发 16 下出现 41.40% 错误率，需要进入后续异常场景或连接资源排查。
- MySQL + Redis 模式的创建短链同样出现约 40% 错误率，说明问题更可能在 MySQL 创建路径或并发资源管理，而不是 Redis 查询缓存。
- Redis hit 和 Redis missing-code 场景 QPS 约为 19，平均延迟约 0.8s，明显慢于纯 MySQL 查询路径；当前 Redis cache 实现每次 get/set 都新建连接，是后续重点排查方向。
- Redis miss 单次请求约 0.41s，只作为单次未命中回源观察，不适合和 500 请求场景直接比较。
- curl fallback 本身会创建大量本地 curl 进程，结果适合做项目内相对比较；正式性能结论仍建议补充 `hey` 基线。

## 结果分析记录

每轮压测后补充：

- 观察到的瓶颈。
- 是否和 Redis 连接创建有关。
- 是否和 MySQL 连接池等待有关。
- 是否和 `server.thread_num` 配置有关。
- 是否需要代码优化，或只需要调整配置。
- 是否需要补充新的测试场景。

## 当前不声明的内容

当前不声明生产或极限性能指标，例如：

- 最大 QPS。
- 最大并发连接数。

这些指标需要在更稳定的压测工具、更多轮次和明确资源观测下再记录。
