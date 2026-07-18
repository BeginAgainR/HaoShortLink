# 压测计划

状态：v1.4 基线已完成，v1.5、v1.6 和 v1.8 已执行代表性回归
当前实现：已完成 curl fallback 与 `hey` 多模式基线、MySQL 创建路径修复、异常场景覆盖、Redis 环境问题定性、Nginx 入口底线验证、v1.4.7 全量回归、v1.5 / v1.6 代表性回归和 v1.8 Kafka 三模式对照

## 说明

本文档记录 HaoShortLink v1.4 的压测目标、工具、环境和结果。v1.4 已在短链接核心接口、
MySQL 持久化、Redis 查询缓存、本地 Compose 编排和第一版测试 / CI 基础上建立可重复的性能与稳定性基线。

## v1.4 目标

- 建立可重复执行的压测入口。
- 记录健康检查、创建短链和短码跳转的基线表现。
- 对比内存存储、MySQL 持久化、MySQL + Redis 查询缓存三种模式。
- 记录 QPS、平均延迟、P95、P99 和错误率。
- 通过压测结果评估 Redis 连接复用、MySQL 连接池参数和 worker 线程数量关系。
- 覆盖关键异常场景，确认服务错误响应稳定。
- 补充 Nginx 入口底线验证，确认反向代理链路在当前版本仍可用。

## v1.4 非目标

- 不声明生产最大承载能力。
- 不接入 Prometheus、Grafana 或完整指标系统。
- 不在没有压测证据时重写连接池。
- 不在 v1.4 实现 Redis 限流；限流进入 v1.6 流量保护阶段。
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

## 压力分级口径

当前项目不把单次本地压测结果视为生产承载能力。为了避免“压力大/小”只凭感觉描述，
后续统一使用以下口径：

| 类型 | 建议参数 | 目标 | 是否可作为性能结论 |
| --- | --- | --- | --- |
| 链路验证 | 并发 1-2，总请求 1-10 | 验证脚本、接口、统计和清理流程能跑通 | 否 |
| 轻量基线 | 并发 8-16，总请求 500-5000 | 观察不同存储模式的相对差异，初步暴露问题 | 可作为阶段内相对比较 |
| 中等压力 | 并发 32-128，总请求 10000-100000 或持续几十秒到数分钟 | 观察 QPS、P95/P99、错误率和资源表现是否稳定 | 需多轮复测后记录 |
| 极限压测 | 逐步升压直到 QPS 不再增长、P99 飙升、错误率上升或资源打满 | 找系统边界和瓶颈 | 不在 v1.4 早期声明 |

判断压力是否已经触发问题，不能只看并发数。至少同时观察：

- QPS 是否随并发提升继续增长。
- 平均延迟、P95、P99 是否明显变差。
- 错误率是否开始上升。
- 服务日志是否出现异常。
- MySQL / Redis / 连接池是否出现等待、超时或连接堆积。

当前 v1.4.2 第一轮属于轻量基线。它已经暴露 MySQL 创建路径错误率和 Redis 查询缓存路径延迟异常，
因此下一步优先解释这些异常，而不是继续盲目升压。

## v1.4.4 异常场景范围

v1.4.4 目标是验证依赖异常或请求异常时的服务稳定性。该阶段不追求最大 QPS，也不把异常场景结果
解释为生产承载能力；重点是确认响应状态、错误码和进程状态可预期。

### 验收口径

- 服务进程不崩溃。
- 请求不出现明显挂死；脚本需要设置请求超时。
- 可预期的客户端错误稳定返回 4xx。
- 依赖异常如导致 5xx，需要稳定返回统一错误响应，并记录是否需要后续优化。
- 异常场景执行后，健康检查仍可响应，除非该场景明确验证的是服务启动失败。
- 发现新的非预期 500、超时或进程退出时，先记录到 `docs/BUGS.md`，再决定是否在 v1.4.4 内修复。

### 场景矩阵

| 场景 | 模式 | 请求 | 期望 | 说明 |
| --- | --- | --- | --- | --- |
| Redis 不可用 | `mysql-redis` | `POST /api/short-links` | `201` | Redis 只是查询缓存，创建路径不依赖 Redis。 |
| Redis 不可用 | `mysql-redis` | `GET /s/{code}` | `302` | Redis get/set 失败时应回源 MySQL，服务不崩溃。 |
| MySQL 不可用 | `mysql` | `POST /api/short-links` | 稳定失败 | 记录服务是启动失败还是请求返回 `500 internal_server_error`。 |
| MySQL 不可用 | `mysql` | `GET /api/health` | 可响应或启动失败需记录 | 先明确当前行为，不在本步承诺健康检查语义。 |
| 非法 URL 高并发 | `memory` / `mysql` / `mysql-redis` | `POST /api/short-links` | `400 invalid_url` | 不应访问 MySQL 或 Redis。 |
| 空 body | `memory` / `mysql` / `mysql-redis` | `POST /api/short-links` | `400 invalid_request` | 验证请求体缺失时错误响应稳定。 |
| 非 JSON body | `memory` / `mysql` / `mysql-redis` | `POST /api/short-links` | `400 invalid_request` | 验证 JSON 解析失败路径。 |
| 缺少 `url` 字段 | `memory` / `mysql` / `mysql-redis` | `POST /api/short-links` | `400 invalid_request` | 验证字段校验路径。 |
| 不存在短码高并发 | `memory` / `mysql` / `mysql-redis` | `GET /s/{code}` | `404 short_link_not_found` | 验证查询未命中路径稳定。 |

### 执行顺序

1. 新增异常场景脚本，复用现有临时配置、临时服务和 curl 检查方式。
2. 先跑链路验证，再跑小并发稳定性验证。
3. 将结果写回本文档和 `docs/ROADMAP.md`。
4. 如发现新 bug，记录到 `docs/BUGS.md` 后再进入修复小循环。

### 当前异常场景入口

```bash
bash tests/scripts/shortlink_exception_scenarios_test.sh
```

脚本默认：

- 使用 `/tmp/haoHTTP-build/shortlink_server`。
- 使用 `curl` 发请求并检查状态码和响应体错误码。
- 每个高并发异常场景请求数为 50。
- 每个高并发异常场景并发数为 8。
- 自动启动临时 `shortlink_server`，结束后清理进程。
- 保留 summary、临时配置、服务日志、响应体和状态码分布。

v1.4.4 首轮验证：

- commit：`c5244e3`
- artifact：`/tmp/haohttp-exception-scenarios.QnWMcG`
- 请求数：每个高并发异常场景 50
- 并发数：8

| 场景 | 模式 | 结果 |
| --- | --- | --- |
| Redis 不可用创建短链 | `mysql-redis` | `201` |
| Redis 不可用短码跳转 | `mysql-redis` | `302`，回源 MySQL 成功 |
| Redis 不可用后健康检查 | `mysql-redis` | `200` |
| MySQL 不可用 | `mysql` | 服务在 ready 前退出，记录为当前稳定行为 |
| 非法 URL 高并发 | `memory` / `mysql` / `mysql-redis` | 每组 `400 50` |
| 空 body 高并发 | `memory` / `mysql` / `mysql-redis` | 每组 `400 50` |
| 非 JSON body 高并发 | `memory` / `mysql` / `mysql-redis` | 每组 `400 50` |
| 缺少 `url` 字段高并发 | `memory` / `mysql` / `mysql-redis` | 每组 `400 50` |
| 不存在短码高并发 | `memory` / `mysql` / `mysql-redis` | 每组 `404 50` |

收口结论：

- v1.4.4 覆盖的异常场景均符合当前预期。
- 本轮未发现新的非预期 500、超时或进程挂死。
- MySQL 不可用时服务在 ready 前退出，这是当前实现的稳定行为；后续如需要运行时降级或更细健康检查语义，另行规划。
- 下一步进入 v1.4.5，优先评估 Redis hit 和 missing-code 路径异常慢的问题。

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
- `HAOHTTP_BENCH_BASE_URL`
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
- `haoHTTP` VM 已安装 `hey v0.1.5`；其他环境未安装时脚本仍会自动回退到 `curl` 模式。
- `hey` 使用 CSV 原始输出计算 QPS、平均延迟、P95、P99 和状态码分布，不依赖摘要展示格式。
- 可通过 `HAOHTTP_BENCH_BASE_URL` 压测已运行的服务或 Nginx 入口；未设置时仍自动启动临时 `shortlink_server`。
- 已使用 `curl` 模式完成小请求量脚本链路验证；该验证只确认脚本可运行，不作为性能基线记录。
- `redirect-cache-hit` 会预热 Redis 后压测跳转。
- `redirect-cache-miss` 会删除 Redis key，并强制只执行 1 个请求，用于观察单次未命中回源成本。

## v1.4.5 连接与资源评估范围

v1.4.5 目标是解释连接和资源使用对当前压测结果的影响，优先处理 Redis 查询缓存路径异常慢的问题。
该阶段先做诊断，不直接预设“必须实现连接池”。

### 待回答问题

- Redis hit 平均延迟约 0.8s 的并发基线，在顺序诊断中约为 0.21s；需要定位服务内固定延迟来源。
- Redis missing-code 平均延迟约 0.8s 的并发基线，在顺序诊断中约为 0.21s；需要定位是否与 Redis cache get miss 路径一致。
- Redis miss 回源 MySQL 的单次成本是多少，和纯 MySQL 查询路径相比是否合理。
- Redis 直连命令本身是否慢，还是 HTTP 服务中的 Redis 使用方式慢。
- 服务内 `RedisShortLinkCache` 的固定延迟来自 `redisConnectWithTimeout`、`GET`、`SETEX`、`redisFree`、地址解析，还是其他 hiredis 使用细节。
- 如果慢点确认与连接创建或 hiredis 使用方式有关，最小修复应采用连接复用、连接池还是调整连接地址 / 超时方式。
- MySQL `mysql.pool_size` 与 `server.thread_num` 的关系是否需要在本阶段继续做参数矩阵。

### 首轮诊断后的判断

首轮诊断显示 Redis 服务本身、Redis 直连命令和直连新建 TCP 连接均为亚毫秒级；纯 MySQL 查询路径也为亚毫秒级。
因此，当前慢点更集中在服务内 `RedisShortLinkCache` 路径。每次请求新建 Redis 连接仍是需要评估的设计问题，
但不能再简单把 0.2s 级固定延迟直接归因为 TCP 新建连接本身。

分段诊断进一步显示，慢点不在 `redisConnectWithTimeout`、地址解析或 `redisFree`，而集中在 hiredis 命令阶段。
在 OrbStack VM 当前网络环境中，`docker.orb.internal` 和显式 IPv4 地址的表现均接近 0.208s；显式使用解析出的 IPv6
字面地址后，hiredis 命令和 HTTP Redis 路径都恢复到亚毫秒级。

### 非目标

- 不在没有诊断数据时重写 Redis cache。
- 不在本步骤实现复杂连接池。
- 不声明生产 Redis 性能指标。
- 不把 curl fallback 结果包装成最终性能结论。
- 不在 Redis 路径原因明确前推进限流实现。

### 执行顺序

1. 新增 Redis cache 诊断入口，记录 Redis 直连命令、HTTP Redis hit、HTTP Redis miss 和 missing-code 的耗时。
2. 在 VM 中执行诊断，记录 artifact、参数和结果。
3. 已分段定位 `RedisShortLinkCache` 内部耗时，确认当前慢点集中在 hiredis 命令阶段，并与 Redis 连接地址选择有关。
4. v1.4.5 优先验证 Redis 连接地址 / 配置策略，再决定是否还需要连接复用或连接池。
5. 如实现修复，复跑 Redis hit、Redis miss、missing-code、Redis 不可用和异常场景脚本。
6. 更新本文档和 `docs/ROADMAP.md`，记录是否还需要 MySQL pool / worker 参数矩阵。

### 当前 Redis 诊断入口

```bash
bash tests/scripts/redis_cache_diagnostic.sh
bash tests/scripts/redis_hiredis_segment_diagnostic.sh
```

`redis_cache_diagnostic.sh` 脚本用途：

- 启动 `storage.type=mysql` 且 `redis.enabled=true` 的临时 `shortlink_server`。
- 创建一条短链并预热 Redis cache。
- 对比 Redis 直连命令和 HTTP Redis 查询缓存路径耗时。
- 删除 Redis key 后测 Redis miss 回源 MySQL 并回填 Redis 的耗时。
- 切换到纯 MySQL 模式，测相同短码跳转和 missing-code 的对照耗时。
- 保留 summary、临时配置、服务日志和每个场景的原始状态 / 延迟文件。

默认参数：

- `HAOHTTP_REDIS_DIAG_REQUESTS=100`
- `HAOHTTP_REDIS_DIAG_PORT=18089`
- `HAOHTTP_REDIS_DIAG_THREAD_NUM=4`
- `HAOHTTP_REDIS_DIAG_MYSQL_POOL_SIZE=4`

`redis_hiredis_segment_diagnostic.sh` 脚本用途：

- 在 artifact 目录中临时生成并编译 C++ hiredis probe。
- 分段记录 `resolve`、`redisConnectWithTimeout`、可选 `SELECT`、`PING` / `GET` / `SETEX`、`redisFree` 的耗时。
- 对比新建连接和复用连接。
- 对比默认 TCP 设置和 `TCP_NODELAY`。
- 保留 summary、临时 C++ probe 源码、probe 二进制和每次迭代的原始 TSV。

默认参数：

- `HAOHTTP_REDIS_SEGMENT_REQUESTS=100`
- `HAOHTTP_REDIS_SEGMENT_CONNECT_TIMEOUT_MS=1000`
- Redis 连接参数沿用 `HAOHTTP_REDIS_HOST`、`HAOHTTP_REDIS_PORT`、`HAOHTTP_REDIS_DATABASE`、`HAOHTTP_REDIS_KEY_PREFIX`。

v1.4.5 首轮诊断：

- commit：`1e73ad3`
- artifact：`/tmp/haohttp-redis-diagnostic.a8BwYK`
- 请求数：每个场景 100
- `server.thread_num=4`
- `mysql.pool_size=4`
- Redis：`docker.orb.internal:16379`

| 场景 | 请求数 | 平均延迟 | P95 | P99 | 错误率 | 备注 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Redis PING new connection | 100 | 0.000259s | 0.000374s | 0.000841s | 0.00% | direct Redis via Python probe |
| Redis GET hit new connection | 100 | 0.000175s | 0.000236s | 0.000289s | 0.00% | direct Redis via Python probe |
| Redis GET hit reused connection | 100 | 0.000025s | 0.000061s | 0.000091s | 0.00% | direct Redis via Python probe |
| Redis GET miss new connection | 100 | 0.000183s | 0.000219s | 0.000314s | 0.00% | direct Redis via Python probe |
| Redis GET miss reused connection | 100 | 0.000044s | 0.000055s | 0.000085s | 0.00% | direct Redis via Python probe |
| HTTP Redis hit | 100 | 0.206670s | 0.210351s | 0.210767s | 0.00% | HTTP curl sequential |
| HTTP Redis miss with MySQL backfill | 100 | 0.415551s | 0.418192s | 0.434207s | 0.00% | Redis key deleted before each request |
| HTTP mysql-redis missing-code | 100 | 0.209700s | 0.212016s | 0.212597s | 0.00% | HTTP curl sequential |
| HTTP MySQL redirect | 100 | 0.000467s | 0.000697s | 0.001041s | 0.00% | HTTP curl sequential |
| HTTP MySQL missing-code | 100 | 0.000413s | 0.000564s | 0.000603s | 0.00% | HTTP curl sequential |

初步判断：

- Redis 服务本身和 Redis 直连命令不是主要瓶颈；新建 Redis TCP 连接的直连探针也是亚毫秒级。
- 慢点集中在服务内 `RedisShortLinkCache` 路径；HTTP Redis hit 和 missing-code 均约 0.21s。
- Redis miss 回源并回填约 0.42s，接近 Redis hit 的两倍，符合一次 GET miss 加一次 SET 回填都走 Redis cache 路径的表现。
- 纯 MySQL redirect 和 missing-code 均约 0.0004s，说明 MySQL 查询路径本身不是该慢点来源。
- 该判断推动 v1.4.5.2 继续分段定位 `RedisShortLinkCache` 内部耗时。

v1.4.5.2 hiredis 分段诊断：

- 诊断入口：`tests/scripts/redis_hiredis_segment_diagnostic.sh`
- 默认地址 artifact：`/tmp/haohttp-redis-segment-diagnostic.JCAwLJ`
- 显式 IPv4 artifact：`/tmp/haohttp-redis-segment-diagnostic.2qwVMs`
- 显式 IPv6 artifact：`/tmp/haohttp-redis-segment-diagnostic.x1ObyU`
- IPv6 HTTP 对照 artifact：`/tmp/haohttp-redis-diagnostic.GlvUy6`

默认地址 `docker.orb.internal:16379`，每场景 100 次：

| 场景 | 请求数 | 平均总耗时 | 平均 connect | 平均 command | 平均 free | 错误率 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| resolve_only | 100 | 0.000164s | 0.000000s | 0.000000s | 0.000000s | 0.00% |
| connect_free | 100 | 0.000151s | 0.000148s | 0.000000s | 0.000003s | 0.00% |
| ping_new_connection | 100 | 0.207969s | 0.000837s | 0.207017s | 0.000114s | 0.00% |
| get_hit_new_connection | 100 | 0.207971s | 0.000917s | 0.206942s | 0.000110s | 0.00% |
| get_miss_new_connection | 100 | 0.207947s | 0.000915s | 0.206904s | 0.000127s | 0.00% |
| setex_new_connection | 100 | 0.207970s | 0.000884s | 0.206966s | 0.000119s | 0.00% |
| get_hit_new_connection_tcp_nodelay | 100 | 0.207970s | 0.000843s | 0.207008s | 0.000116s | 0.00% |

显式 IPv4 `192.168.139.2:16379`，每场景 100 次：

| 场景 | 请求数 | 平均总耗时 | 平均 connect | 平均 command | 平均 free | 错误率 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ping_new_connection | 100 | 0.207925s | 0.000250s | 0.207558s | 0.000115s | 0.00% |
| get_hit_new_connection | 100 | 0.207966s | 0.000256s | 0.207592s | 0.000117s | 0.00% |
| setex_new_connection | 100 | 0.207971s | 0.000268s | 0.207588s | 0.000114s | 0.00% |

显式 IPv6 `fd07:b51a:cc66::2:16379`，每场景 20 次：

| 场景 | 请求数 | 平均总耗时 | 平均 connect | 平均 command | 平均 free | 错误率 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ping_new_connection | 20 | 0.000087s | 0.000012s | 0.000068s | 0.000007s | 0.00% |
| get_hit_new_connection | 20 | 0.000061s | 0.000033s | 0.000013s | 0.000015s | 0.00% |
| get_miss_new_connection | 20 | 0.000081s | 0.000030s | 0.000035s | 0.000015s | 0.00% |
| setex_new_connection | 20 | 0.000079s | 0.000021s | 0.000043s | 0.000014s | 0.00% |

显式 IPv6 后的 HTTP 对照，每场景 20 次：

| 场景 | 请求数 | 平均延迟 | P95 | P99 | 错误率 |
| --- | ---: | ---: | ---: | ---: | ---: |
| HTTP Redis hit | 20 | 0.000301s | 0.000446s | 0.000489s | 0.00% |
| HTTP Redis miss with MySQL backfill | 20 | 0.000637s | 0.000858s | 0.000903s | 0.00% |
| HTTP mysql-redis missing-code | 20 | 0.000551s | 0.000734s | 0.000785s | 0.00% |

分段判断：

- `resolve`、`redisConnectWithTimeout`、`redisFree` 都不是 0.2s 固定延迟来源。
- `GET`、`SETEX` 和 `PING` 在 `docker.orb.internal` / 显式 IPv4 下均出现约 0.208s 命令阶段等待。
- `TCP_NODELAY` 对照没有消除该等待。
- 显式 IPv6 地址下 hiredis 命令恢复到亚毫秒级，HTTP Redis hit 也从约 0.207s 降至约 0.0003s。
- 下一步应优先验证 Redis 连接地址 / 配置策略；连接复用和连接池仍是设计优化项，但不是解释当前 0.2s 固定延迟的第一修复点。

v1.4.5 收口计划：

- 目标：将当前 Redis 慢路径定性为本地 OrbStack VM 访问 Docker Redis 发布端口的环境特定网络路径问题，并验证本地 VM 连接地址策略能否稳定消除当前约 0.2s 固定延迟。
- 推荐先使用当前环境解析出的 IPv6 字面地址作为对照值，不把该地址写入公开默认配置。
- 复跑 `redis_cache_diagnostic.sh`，覆盖 Redis hit、Redis miss 回源、missing-code 和纯 MySQL 对照。
- 复跑 `redis_hiredis_segment_diagnostic.sh`，确认 hiredis `PING` / `GET` / `SETEX` 命令阶段保持亚毫秒级。
- 复跑 Redis 不可用 fallback 和异常场景脚本，确认地址策略不改变业务语义。
- 如结果稳定，将本地 VM 推荐配置写入 `docs/RUNBOOK.md`；如结果不稳定，再评估连接复用、连接池、其他网络配置或换部署环境复测。
- 本阶段不声明生产性能指标，也不把 OrbStack 本地网络现象外推到线上环境。
- IPv6 地址策略属于本地 VM 访问 OrbStack Docker 发布端口的地址路径规避，不是业务代码层面的 Redis 修复；容器内运行时仍优先通过 Compose 服务名访问 Redis。

v1.4.5 验证结果：

- commit：`bdb76ba`
- Redis IPv6：`fd07:b51a:cc66::2:16379`
- hiredis 分段 artifact：`/tmp/haohttp-redis-segment-diagnostic.GJ3AX3`
- Redis cache HTTP artifact：`/tmp/haohttp-redis-diagnostic.xdmv2Q`
- 异常场景 artifact：`/tmp/haohttp-exception-scenarios.4FvNKL`

| 场景 | 请求数 | 平均延迟 | P95 | P99 | 错误率 |
| --- | ---: | ---: | ---: | ---: | ---: |
| hiredis GET hit new connection | 100 | 0.000068s | 0.000094s | 0.000102s | 0.00% |
| hiredis SETEX new connection | 100 | 0.000073s | 0.000101s | 0.000119s | 0.00% |
| HTTP Redis hit | 100 | 0.000304s | 0.000427s | 0.000462s | 0.00% |
| HTTP Redis miss with MySQL backfill | 100 | 0.000769s | 0.001042s | 0.002549s | 0.00% |
| HTTP mysql-redis missing-code | 100 | 0.000633s | 0.000888s | 0.000929s | 0.00% |

补充验证：

- `tests/scripts/redis_unavailable_fallback_test.sh` 通过：Redis 不可用时创建短链成功，跳转可回源 MySQL。
- `tests/scripts/shortlink_exception_scenarios_test.sh` 通过：Redis 不可用、MySQL 不可用、非法 URL、空 body、非 JSON、缺少 `url` 和短码不存在场景结果稳定。

结论：

- 本地 VM 使用 IPv6 地址访问 OrbStack Docker Redis 发布端口后，HTTP Redis hit / miss / missing-code 不再出现约 0.2s 固定延迟。
- ENV-001 属于本地验证环境的网络路径问题，不作为业务代码 bug 或生产性能结论。
- v1.4 不实现 Redis 连接池；连接池后续仅在新的压测证据表明需要时再评估。

v1.4.6 验证结果：

- 环境：OrbStack Linux VM `haoHTTP`，Linux arm64。
- 工具：`hey v0.1.5`，使用 Ubuntu `golang-go` 提供的 Go 1.26.0，通过 `GOBIN="$HOME/.local/bin" go install github.com/rakyll/hey@v0.1.5` 安装。
- commit：`5cebf70` 加当前未提交的压测脚本改动。
- 常规场景：并发 16，总请求 1000；Redis miss 只执行 1 个请求。
- `server.thread_num=4`，`mysql.pool_size=4`。
- VM 手工运行时 Redis 使用本环境 IPv6 对照地址；Compose 容器内仍使用 `redis:6379`。

直连代表结果：

| 场景 | 模式 | 并发 | 请求数 | QPS | 平均延迟 | P95 | P99 | 错误率 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| GET /api/health | memory | 16 | 1000 | 57674.42 | 0.000216s | 0.0013s | 0.0023s | 0.00% |
| POST /api/short-links | memory | 16 | 1000 | 56045.20 | 0.000262s | 0.0016s | 0.0032s | 0.00% |
| POST /api/short-links | mysql | 16 | 1000 | 1270.17 | 0.011369s | 0.0240s | 0.0373s | 0.00% |
| GET /s/{code} | memory | 16 | 1000 | 70857.14 | 0.000204s | 0.0011s | 0.0031s | 0.00% |
| GET /s/{code} | mysql | 16 | 1000 | 9668.62 | 0.001509s | 0.0042s | 0.0085s | 0.00% |
| GET /s/{code} Redis hit | mysql-redis | 16 | 1000 | 19604.74 | 0.000748s | 0.0014s | 0.0026s | 0.00% |
| GET /s/{code} Redis miss | mysql-redis | 1 | 1 | 1000.00 | 0.001000s | 0.0010s | 0.0010s | 0.00% |
| POST invalid URL | memory | 16 | 1000 | 49353.23 | 0.000255s | 0.0017s | 0.0027s | 0.00% |
| GET missing short code | mysql-redis | 16 | 1000 | 8817.78 | 0.001586s | 0.0029s | 0.0038s | 0.00% |

Nginx 入口验证使用当前工作区重新构建 `hao-shortlink-server:dev`，由 VM 客户端访问 OrbStack Docker 发布的 Nginx `8080` 端口。`docker.orb.internal` 在并发 `hey` 下再次出现约 0.21s P95；改用当前环境 IPv6 字面地址后结果如下：

| 场景 | 模式 | 并发 | 请求数 | QPS | 平均延迟 | P95 | P99 | 错误率 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| GET /api/health | mysql-redis | 16 | 1000 | 16957.26 | 0.000898s | 0.0025s | 0.0041s | 0.00% |
| POST /api/short-links | mysql-redis | 16 | 1000 | 1715.67 | 0.008783s | 0.0190s | 0.0227s | 0.00% |
| GET /s/{code} | mysql-redis | 16 | 1000 | 13589.04 | 0.001141s | 0.0027s | 0.0033s | 0.00% |
| GET /s/{code} Redis hit | mysql-redis | 16 | 1000 | 12353.67 | 0.001239s | 0.0031s | 0.0042s | 0.00% |
| GET /s/{code} Redis miss | mysql-redis | 1 | 1 | 1111.11 | 0.000900s | 0.0009s | 0.0009s | 0.00% |
| POST invalid URL | mysql-redis | 16 | 1000 | 22142.86 | 0.000644s | 0.0016s | 0.0029s | 0.00% |
| GET missing short code | mysql-redis | 16 | 1000 | 9910.09 | 0.001549s | 0.0030s | 0.0042s | 0.00% |

结论：

- 直连和 Nginx IPv6 入口覆盖的核心场景均无错误，Nginx 反向代理底线成立。
- `hey` 基线确认 BUG-004 修复有效，MySQL 创建路径在本轮 1000 请求中无非 201 响应。
- Redis hit 明显快于纯 MySQL redirect，但该结果只用于当前环境的相对比较。
- Nginx 入口存在可测量开销，但本轮不做极限压测，也不声明生产最大承载能力。
- VM 到 Docker 发布端口的地址选择仍会显著影响结果，后续本地基线必须记录并固定访问地址。

v1.4.7 验证结果：

- commit：`930ecc5`。
- Linux VM Release 构建通过，CTest `1/1` 通过。
- Linux VM 独立干净克隆验证通过，目录为 `/tmp/haoHTTP-v1.4-clean.fjetXo`，未复用日常构建目录。
- API smoke 通过：健康检查、创建短链、302 跳转、短码不存在和非法 URL 均符合预期。
- Compose-backed MySQL / Redis 集成测试通过：MySQL 持久化、Redis miss 回源和缓存回填正常。
- Redis 不可用 fallback 通过：创建短链返回 `201`，跳转可回源 MySQL 并返回 `302`。
- 异常场景 artifact：`/tmp/haohttp-exception-scenarios.EUZdzv`。
- 异常场景中，Redis 不可用 fallback 和 MySQL 不可用启动失败行为稳定；三种存储模式下每组 50 个非法 URL、空 body、非 JSON、缺少 `url` 和短码不存在请求均返回预期 4xx。

上述 `/tmp` 路径是本地临时验证产物，不属于仓库中的长期 artifact；长期结论以本文记录的参数、结果表和摘要为准。

v1.4 收口结论：

- BUG-004 已修复且在 curl、`hey` 和集成回归中未再出现。
- ENV-001 是 OrbStack VM 到 Docker 发布端口的本地网络路径问题；本地性能基线必须记录并固定访问地址，不外推为生产结论。
- 当前没有证据要求在 v1.4 实现 Redis 连接池。
- MySQL 连接池参数矩阵、worker 数量关系和连接等待超时后置到出现新的连接等待或资源瓶颈证据时再评估。
- Redis 限流进入 v1.6；可观测性进入 v1.5。
- v1.4 不声明生产最大承载能力。

## v1.5.4 可观测性性能回归

本轮只比较 request ID、通用请求日志和进程内指标引入前后的代表性低依赖路径，不重新建立完整容量基线，
也不把本地结果解释为生产承载能力。对比使用同一个 Linux VM、同一份压测脚本和相同参数，两个版本
均从独立干净克隆执行 Release 构建。

环境与参数：

- 环境：OrbStack Linux VM `haoHTTP`，Linux arm64。
- 对照版本：v1.4.0 tag，commit `ba639a9`。
- 观测版本：v1.5.3，commit `6be60cc`；v1.5.4 不修改请求处理代码。
- 工具：`hey v0.1.5`。
- 模式：内存存储，`server.thread_num=4`。
- 参数：并发 16，每轮 20000 请求，每个版本 / 场景三轮，按版本交替执行。
- 临时目录：`/tmp/haoHTTP-v1.5.4-perf.GcNGbo`；该目录不是长期 artifact。

结果：

| 场景 | 版本 | 三轮 QPS | 中位 QPS | 中位平均延迟 | P95 范围 | P99 范围 | 错误率 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| `GET /api/health` | v1.4.0 | 66622.25 / 76893.50 / 78492.94 | 76893.50 | 0.000188s | 0.0013s–0.0014s | 0.0024s–0.0026s | 0.00% |
| `GET /api/health` | v1.5 | 81070.13 / 83752.09 / 89405.45 | 83752.09 | 0.000169s | 0.0011s–0.0014s | 0.0022s–0.0025s | 0.00% |
| `GET /s/{code}` | v1.4.0 | 76599.00 / 77972.71 / 77730.28 | 77730.28 | 0.000189s | 0.0014s–0.0015s | 0.0024s–0.0027s | 0.00% |
| `GET /s/{code}` | v1.5 | 70348.22 / 72150.07 / 71275.84 | 71275.84 | 0.000208s | 0.0016s | 0.0024s–0.0027s | 0.00% |

结论：

- health 三轮未观察到性能回归；其 QPS 变化更可能反映本地短时调度波动，不能解释为观测代码带来提升。
- memory redirect 的中位 QPS 约下降 8.3%，中位平均延迟约增加 10.1%；绝对平均延迟增加约 0.019ms，P99 范围没有扩大，三轮错误率均为 0。
- 该对比记录 request ID 生成、单行请求日志、状态分类、counter 和 histogram 更新的合并影响，不能从中单独归因某个组件。
- 本轮说明极低延迟内存路径存在可测量观测开销，但没有出现错误、P99 放大或依赖路径退化证据；v1.5 记录该基线，不据此提前改写日志或指标实现。
- 后续若在真实工作负载、持续压测或资源观测中出现新的瓶颈证据，再单独评估异步日志、采样策略或指标更新成本。

## v1.6.6 可靠性与限流性能回归

本轮只对比 v1.6 改动前后的低依赖路径。限流保持默认关闭，因此结果用于确认健康路由、配置和限流组件接入后
没有影响未开启限流的现有请求路径，不表示 Redis 限流开启时的性能成本，也不作为生产承载承诺。

环境与参数：

- 日期：2026-07-15。
- 环境：OrbStack Linux VM `haoHTTP`，Linux arm64。
- 对照版本：`main`，commit `d601e7d`。
- v1.6 版本：实现内容对应 commit `3d276c0`；后续 `ced354a` 只调整干净克隆测试编排。
- 工具：`hey v0.1.5`。
- 模式：内存存储，`rate_limit.enabled=false`，`server.thread_num=4`。
- 参数：并发 16，每轮 20000 请求，每个版本 / 场景三轮。
- 对照临时源码目录：`/tmp/haoHTTP-v1.5-baseline.nX32XF`。
- 对照构建目录：`/tmp/haoHTTP-v1.5-build.MJaib8`。

结果：

| 场景 | 版本 | 三轮 QPS | 中位 QPS | 中位平均延迟 | P95 范围 | P99 范围 | 错误率 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| `GET /api/health` | `main` | 87950.75 / 90212.00 / 90456.81 | 90212.00 | 0.000157s | 0.0010s–0.0011s | 0.0021s–0.0023s | 0.00% |
| `GET /api/health` | v1.6 | 82542.30 / 86505.19 / 88731.14 | 86505.19 | 0.000164s | 0.0010s–0.0012s | 0.0022s | 0.00% |
| `GET /s/{code}` | `main` | 67659.00 / 66006.60 / 78003.12 | 67659.00 | 0.000219s | 0.0014s–0.0016s | 0.0024s–0.0029s | 0.00% |
| `GET /s/{code}` | v1.6 | 74710.50 / 72176.11 / 74487.90 | 74487.90 | 0.000196s | 0.0014s–0.0015s | 0.0024s–0.0027s | 0.00% |

结论：

- health 中位 QPS 约下降 4.1%，中位平均延迟增加 0.007ms；P99 没有扩大，三轮错误率均为 0。
- memory redirect 中位 QPS 约上升 10.1%，中位平均延迟下降约 0.023ms；该变化应视为本地短时调度波动，不解释为优化效果。
- 本轮未观察到错误、P99 放大或明显性能回退，因此不根据该数据进行新的日志、指标或连接管理优化。

## 当前诊断入口

针对 BUG-004，当前新增 MySQL 创建路径并发诊断脚本：

```bash
bash tests/scripts/mysql_create_concurrency_diagnostic.sh
```

脚本用途：

- 启动 `storage.type=mysql` 的临时 `shortlink_server`。
- 按并发阶梯压 `POST /api/short-links`。
- 保留每档状态码分布。
- 保留响应体样本。
- 保留 `shortlink_server` 日志和临时配置。
- 清理本次诊断插入的 MySQL 测试数据。

默认参数：

- `HAOHTTP_CREATE_DIAG_REQUESTS=100`
- `HAOHTTP_CREATE_DIAG_CONCURRENCY_LEVELS="1 2 4 8 16"`
- `HAOHTTP_CREATE_DIAG_THREAD_NUM=4`
- `HAOHTTP_CREATE_DIAG_MYSQL_POOL_SIZE=4`
- `HAOHTTP_CREATE_DIAG_PORT=18086`

v1.4.3 阶梯诊断结果：

- commit：`51dca81`
- artifact：`/tmp/haohttp-create-diagnostic.96ByrP`
- 请求数：每档 100
- 并发档位：1、2、4、8、16
- `server.thread_num=4`
- `mysql.pool_size=4`

| 场景 | 并发 | 请求数 | 201 | 500 | 错误率 | 结论 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `POST /api/short-links` | 1 | 100 | 70 | 30 | 30.00% | 并发 1 已可复现失败 |
| `POST /api/short-links` | 2 | 100 | 64 | 36 | 36.00% | 失败不是单纯连接池耗尽 |
| `POST /api/short-links` | 4 | 100 | 58 | 42 | 42.00% | 错误率接近 v1.4.2 基线 |
| `POST /api/short-links` | 8 | 100 | 50 | 50 | 50.00% | 大小写短码冲突持续出现 |
| `POST /api/short-links` | 16 | 100 | 72 | 28 | 28.00% | 仍为 500 唯一键冲突 |

服务日志中的失败原因是 MySQL 唯一键冲突：

```text
Duplicate entry '0000Gw' for key 'short_links.uk_short_links_code'
```

根因判断：`short_links.code` 采用大小写敏感 Base62 短码，但 MySQL 表默认排序规则是
`utf8mb4_0900_ai_ci`，唯一索引大小写不敏感；因此 `0000GW` 与 `0000Gw` 会被 MySQL 判定为重复。
下一步先修复数据模型或短码策略，再复跑诊断和基线。

v1.4.3 修复后复测：

- 修复方式：`short_links.code` 显式使用 `utf8mb4_bin`；已有表通过
  `002_make_short_link_code_case_sensitive.sql` 迁移。
- 诊断 artifact：`/tmp/haohttp-create-diagnostic.CJl2iG`

| 场景 | 并发 | 请求数 | 201 | 非 201 | 错误率 | 结论 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `POST /api/short-links` | 1 | 100 | 100 | 0 | 0.00% | 修复后并发 1 通过 |
| `POST /api/short-links` | 2 | 100 | 100 | 0 | 0.00% | 修复后并发 2 通过 |
| `POST /api/short-links` | 4 | 100 | 100 | 0 | 0.00% | 修复后并发 4 通过 |
| `POST /api/short-links` | 8 | 100 | 100 | 0 | 0.00% | 修复后并发 8 通过 |
| `POST /api/short-links` | 16 | 100 | 100 | 0 | 0.00% | 修复后并发 16 通过 |

核心 create 基线复测：

| 场景 | 模式 | 并发 | 总请求/时长 | QPS | 平均延迟 | P95 | P99 | 错误率 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| POST /api/short-links | mysql | 16 | 1000 requests | 1100.11 | 0.011569s | 0.017904s | 0.020231s | 0.00% | tool curl; expected 201; v1.4.3 fix |
| POST /api/short-links | mysql-redis | 16 | 1000 requests | 1094.09 | 0.011559s | 0.025230s | 0.029734s | 0.00% | tool curl; expected 201; v1.4.3 fix |

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

## 异常场景收口

- Redis 不可用时，创建短链和短码跳转均符合预期；跳转可回源 MySQL。
- MySQL 不可用时，当前服务在 ready 前退出，已记录为当前稳定行为。
- 非法 URL、空 body、非 JSON body、缺少 `url` 字段在三种模式下均稳定返回 `400`。
- 不存在短码在三种模式下均稳定返回 `404`。
- v1.4.4 不做连接优化，连接与资源问题进入 v1.4.5。

## 结果记录格式

压测结果使用表格记录，示例格式如下。具体实测结果按阶段追加到对应章节。

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
- curl fallback 本身会创建大量本地 curl 进程，该轮结果只适合做项目内相对比较；v1.4.6 已补充 `hey` 小基线。

后续顺序：

1. BUG-004 已定位并修复，`mysql create` 和 `mysql-redis create` 复测错误率均为 0.00%。
2. v1.4.4 异常场景补强已完成，未发现新的非预期 500、超时或进程挂死。
3. v1.4.5 已将 Redis 固定延迟定性为本地 VM 到 Docker 发布端口的地址路径问题，并验证 IPv6 规避策略。
4. v1.4.6 已完成 `hey` 核心小基线和 Nginx 入口底线验证，覆盖场景错误率均为 0.00%。
5. v1.4.7 全量回归和版本收口已完成；`server.thread_num` / `mysql.pool_size` 参数矩阵后置到有新证据时再评估。

## v1.8 Kafka 三模式对照

本轮用于确认异步访问事件不会给跳转路径带来稳定可复现的回归，不声明生产承载上限。

环境与参数：

- OrbStack Linux VM `haoHTTP`，Release 构建目录 `/tmp/haoHTTP-build`。
- 内存存储，预先创建同一个短链，场景为 `GET /s/{code}`。
- 工具 `hey`，每轮 20000 请求，并发 16；每种 Kafka 模式执行 3 轮。
- `disabled`：不创建 producer；`normal`：连接本地 Kafka；`failure`：连接不可用 broker，验证 fail-open。

结果：

| 轮次 | Kafka 模式 | QPS | 平均延迟 | P95 | P99 | 错误率 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | disabled | 53304.90 | 0.000290s | 0.0018s | 0.0029s | 0.00% |
| 1 | normal | 59014.46 | 0.000256s | 0.0017s | 0.0026s | 0.00% |
| 1 | failure | 59364.80 | 0.000259s | 0.0017s | 0.0028s | 0.00% |
| 2 | disabled | 72020.17 | 0.000209s | 0.0013s | 0.0023s | 0.00% |
| 2 | normal | 64703.98 | 0.000232s | 0.0016s | 0.0027s | 0.00% |
| 2 | failure | 57290.17 | 0.000269s | 0.0017s | 0.0028s | 0.00% |
| 3 | disabled | 55279.16 | 0.000276s | 0.0017s | 0.0029s | 0.00% |
| 3 | normal | 64020.49 | 0.000237s | 0.0017s | 0.0029s | 0.00% |
| 3 | failure | 58651.03 | 0.000263s | 0.0017s | 0.0027s | 0.00% |

三轮中位数：

| Kafka 模式 | QPS | 平均延迟 | P95 | P99 | 错误率 |
| --- | ---: | ---: | ---: | ---: | ---: |
| disabled | 55279.16 | 0.000276s | 0.0017s | 0.0029s | 0.00% |
| normal | 64020.49 | 0.000237s | 0.0017s | 0.0027s | 0.00% |
| failure | 58651.03 | 0.000263s | 0.0017s | 0.0028s | 0.00% |

结论：三组均无 HTTP 错误，Kafka 正常和故障模式没有出现依赖 broker RTT 的固定等待，也没有观察到
稳定可复现的延迟回归。单机短时 QPS 波动明显，因此只用于证明当前 fail-open 路径的阶段内相对表现。

复现入口：

```bash
HAOHTTP_BENCH_SCENARIO=redirect \
HAOHTTP_BENCH_MODE=memory \
HAOHTTP_BENCH_KAFKA_MODE=disabled \
HAOHTTP_BENCH_REQUESTS=20000 \
HAOHTTP_BENCH_CONCURRENCY=16 \
bash tests/scripts/benchmark_shortlink.sh
```

将 `HAOHTTP_BENCH_KAFKA_MODE` 依次改为 `normal` 和 `failure` 完成三模式对照。

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
