# 可观测性设计

状态：v1.5.3 已完成
当前实现：已接入 request ID、通用结构化请求日志、进程内指标、`/metrics`，以及本地 Prometheus、Grafana 和最小 dashboard

## 目标

v1.5 让 `shortlink_server` 的运行状态可以被观察和排查，并为 v1.6 的可靠性与流量保护提供证据。

第一版覆盖：

- 为请求建立可关联的 `request_id`。
- 整理通用 HTTP 日志和短链接业务日志字段。
- 记录请求量、错误率、延迟、创建 / 跳转结果和缓存行为等基础指标。
- 提供 Prometheus 可抓取的 `/metrics` 入口。
- 在本地 Compose 中接入 Prometheus、Grafana 和最小 dashboard。
- 为日志、指标和抓取链路补充自动化验证。

## 非目标

v1.5 不处理：

- Redis 限流或 `429 Too Many Requests`。
- liveness / readiness 拆分和完整依赖健康语义；该能力进入 v1.6。
- OpenTelemetry、分布式追踪或跨服务 trace。
- ELK、Loki 等集中式日志平台。
- 生产告警、值班和长期容量规划。
- 把本地压测数据声明为生产承载能力。
- 重写 `HttpServer/`、Redis 或 MySQL 连接管理。

## 分层边界

### 框架层

`HttpServer/` 只记录可复用的 HTTP 维度：

- `request_id`
- method
- 路由模板
- status
- duration
- 通用错误结果

框架层不理解短码、原始 URL、Redis hit / miss 或 MySQL repository 等业务概念。

### 应用层

`apps/shortlink_server/` 记录短链接业务维度：

- 创建成功、校验失败或存储失败
- 跳转成功、短码不存在、禁用或过期等业务结果
- 查询来源为 memory、MySQL 或 Redis
- Redis get / set 的 hit、miss、success 或 error
- MySQL / Redis 操作错误

## Request ID

第一版约束：

- 每个进入服务的 HTTP 请求都应有一个非空的 opaque `request_id`；它只用于关联日志，不承担认证、授权或其他安全凭证语义。
- `X-Request-ID` 头名称按 HTTP 语义大小写无关读取。
- 客户端 request ID 长度为 1 到 64 个字符，只接受 ASCII 字母、数字、点号、下划线和连字符；不满足约束时忽略原值并由服务重新生成。
- 服务端生成的 request ID 固定为 32 个小写十六进制字符，由进程级随机 nonce 和原子递增序号组合；随机源不可用时退化为时间派生 nonce 和原子序号，生成失败不能导致业务请求失败。
- 服务生成或接受的 request ID 应写入响应头 `X-Request-ID`。
- 同一请求的框架日志、业务日志和错误日志使用相同 request ID。
- request ID 只用于日志关联，不作为 Prometheus label。
- request ID 不用于全局唯一性、安全随机性、认证或幂等保证。

## 请求上下文与路由模板

第一版不引入独立的通用 context 框架，只在现有请求处理链中传递最小观测元数据：

- `HttpServer` 在进入 middleware 和 Router 前确定 request ID，业务回调收到的 `HttpRequest` 可以读取同一 ID。
- `Router` 在匹配成功时返回注册时的路由模板；精确路由返回原注册路径，动态路由返回 `/s/:code` 这样的原始模板。
- 未匹配路由统一记录为有限值 `unmatched`，不把原始请求路径退化为 route 字段。
- middleware 提前返回和自定义 HTTP callback 分别使用有限值 `middleware`、`custom_callback`。
- 路由模板作为 Router 的匹配结果显式返回，不通过全局变量或线程局部变量隐式传递。
- 动态路由产生的请求副本必须保留 request ID；请求 reset / swap 后不得残留上一请求的观测元数据。
- 响应头和请求完成日志统一在 `HttpServer` 发送响应前补齐，避免要求 middleware 的 `after()` 接口承担请求关联。

该方案允许在 v1.5.1 内对 `HttpServer/` 做局部扩展，但不改变业务路由的 HTTP 语义，也不重写现有 middleware 接口。

## 结构化日志

第一版使用单行稳定 key-value 格式，沿用 muduo 提供的时间戳和日志级别前缀，不额外引入 JSON 日志依赖。示例：

```text
event=http_request request_id=0123456789abcdef0000000000000001 method=GET route=/s/:code status=302 duration_ms=0.241
```

字段顺序保持稳定，字段值必须经过约束或转义，日志验证只依赖字段语义，不依赖终端颜色或自由文本。

### 通用请求日志字段

| 字段 | 含义 |
| --- | --- |
| `timestamp` | 由 muduo 日志前缀提供的记录时间 |
| `level` | 由 muduo 日志前缀提供的日志级别 |
| `event` | 稳定事件名，例如 `http_request` |
| `request_id` | 请求关联标识 |
| `method` | HTTP 方法 |
| `route` | 路由模板，例如 `/s/:code`，不记录原始高基数路径 |
| `status` | HTTP 状态码 |
| `duration_ms` | 请求处理耗时 |
| `error_code` | 可选的稳定错误码 |

第一批稳定事件名：

- `http_request`：请求完成后的通用请求日志，每个已完成请求只记录一次。
- `http_parse_error`：请求解析失败，服务无法取得可信路由信息时记录。
- `shortlink_create`：创建短链业务结果。
- `shortlink_redirect`：短码跳转业务结果。
- `shortlink_cache`：Redis get / set 结果。
- `shortlink_backend_error`：MySQL 或 Redis 操作错误。

业务事件按实际执行位置逐步接入；v1.5.1 至少完成 `http_request`，其余事件与对应指标埋点一起补齐，但字段命名遵循本设计。

### 业务日志字段

| 字段 | 含义 |
| --- | --- |
| `operation` | `create`、`redirect`、`cache_get`、`cache_set` 等有限集合 |
| `result` | `success`、`invalid`、`not_found`、`error` 等有限集合 |
| `storage` | `memory` 或 `mysql` |
| `cache_result` | `hit`、`miss`、`success`、`error` |
| `backend` | `mysql` 或 `redis` |

### 敏感信息边界

默认不得记录：

- MySQL、Redis、Kafka 密码或完整连接串。
- Authorization、Cookie 或其他凭据。
- 完整请求体。
- 完整原始 URL 或其中的 query 参数。
- 短码、IP、User-Agent 等高基数值作为指标标签。

日志如确需记录短码、IP 或 User-Agent，必须先明确排障价值、保留周期和脱敏方式。

## 指标

第一版指标名称和标签已按以下定义实现。

### HTTP 指标

```text
haohttp_http_requests_total{method,route,status_class}
haohttp_http_request_duration_seconds{method,route}
```

### 短链接业务指标

```text
haohttp_shortlink_create_total{result,storage}
haohttp_shortlink_redirect_total{result,source}
haohttp_shortlink_cache_operations_total{operation,result}
haohttp_shortlink_backend_errors_total{backend,operation}
```

其中：

- `method`、`route`、`status_class`、`result`、`storage`、`source`、`operation` 和 `backend` 必须来自有限集合。
- `route` 使用路由模板，不使用 `/s/000001` 等实际路径。
- `status_class` 优先使用 `2xx`、`3xx`、`4xx`、`5xx`，避免无意义扩张。
- 不允许使用 request ID、短码、原始 URL、IP、User-Agent 或错误消息作为 label。

### 延迟指标

请求延迟使用 histogram，单位为秒。第一版 bucket 为：

```text
0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05,
0.1, 0.25, 0.5, 1, 2.5, 5, +Inf
```

该范围覆盖现有亚毫秒本地快路径，并保留到秒级异常请求的观察空间。

## `/metrics` 暴露

第一版约束：

- `GET /metrics` 返回 Prometheus text exposition format，Content-Type 为 `text/plain; version=0.0.4; charset=utf-8`。
- 指标入口通过 `metrics.enabled` 启用或关闭，第一版默认启用；关闭时不注册该路由。
- 本地 Compose 中由 Prometheus 直接访问服务内部端口。
- Nginx 默认不把 `/metrics` 暴露为公开入口。
- scrape 读取必须线程安全，不能清零累计指标，也不能明显阻塞业务线程。
- 指标构造失败应返回稳定错误，不影响其他业务路由。

## 线程安全与开销

- 多个 muduo worker 线程可能并发更新同一指标。
- 计数器优先使用原子或低竞争实现；histogram 更新方式需要单独验证。
- 不在每次请求中执行磁盘 I/O、网络上报或高开销动态标签注册。
- Prometheus 抓取读取快照时，不应长时间持有影响请求路径的全局锁。
- v1.5 完成后使用现有 `hey` 小基线做相对回归，不声明生产承载能力。

当前实现中，HTTP route 在服务启动注册路由时预注册；特殊 callback / unmatched 情况只在首次出现时注册有限固定值。请求路径更新使用原子计数器，route 查找使用共享锁。Prometheus scrape 读取原子快照，不清零累计值；并发更新期间不同字段可能存在极短暂的采样时差，但不会阻塞整个请求处理过程。

## Prometheus 与 Grafana

本地 Compose 已实现：

```text
shortlink_server -> /metrics -> Prometheus -> Grafana
```

最小 dashboard 展示：

- QPS。
- 4xx / 5xx 比例。
- 请求平均延迟和 P95 / P99。
- 创建与跳转结果。
- Redis cache hit / miss / error。
- MySQL / Redis backend error。

dashboard 只展示已定义的低基数聚合，不以短码或用户维度展开。

当前本地配置：

- Prometheus 每 `5s` 从 Compose 内网抓取 `shortlink_server:8080/metrics`。
- Prometheus 数据保留时间为 `15d`，写入 `hao_shortlink_prometheus_data` named volume。
- Grafana 自动配置 UID 为 `hao-prometheus` 的 Prometheus 数据源。
- Grafana 自动从仓库中的 JSON 加载 `HaoShortLink Overview` dashboard。
- Prometheus `9090` 和 Grafana `3000` 只绑定宿主机 `127.0.0.1`。
- Grafana 关闭匿名访问和用户注册，首次初始化默认管理员为 `admin` / `admin`，可通过环境变量覆盖。
- Grafana 运行状态写入 `hao_shortlink_grafana_data` named volume；数据源和 dashboard 文件仍以 Git 中的 provisioning 配置为准。

当前不引入 Alertmanager、生产级鉴权、Prometheus 高可用或远程长期存储。`docker compose down`
默认保留监控数据卷；`docker compose down -v` 会连同 MySQL、Redis 和监控数据一起删除。

## 验证计划

### 单元与框架测试

- 请求具备 request ID，响应头返回同一 ID。
- 客户端 request ID 的合法与非法输入行为稳定。
- 计数器、状态分类和路由模板记录正确。
- histogram bucket 和累计计数正确。
- 并发更新不丢失计数，不触发数据竞争或崩溃。

### API 与集成测试

- `/metrics` 返回 Prometheus 文本格式。
- health、create、redirect、invalid-url、missing-code 后指标按预期变化。
- Redis hit、miss、error 和 MySQL error 指标可区分。
- Prometheus 能在 Compose 网络中成功 scrape。
- Grafana 能连接 Prometheus 并加载最小 dashboard。
- Nginx 公开入口默认不能访问 `/metrics`。

### 回归

- Linux 构建、CTest、API smoke 和 MySQL / Redis 依赖 CI 继续通过。
- 复跑代表性 `hey` 小基线，观察观测代码引入前后的相对差异。
- 不把一次本地性能结果作为生产指标。

## v1.5 执行批次

1. v1.5.0 设计收口：
   - 状态：已完成。
   - 已确定日志字段、request ID、最小请求上下文、路由模板、指标标签和暴露边界。
2. v1.5.1 request ID 与结构化日志：
   - 状态：已完成。
   - 已实现 request ID 校验 / 生成、请求链路传递和响应头回传。
   - 已实现 Router 路由模板匹配结果、通用 `http_request` 日志和解析错误日志。
   - 已补充 request ID、并发生成、大小写无关请求头、路由模板单元测试和 API smoke 验证。
3. v1.5.2 进程内指标与 `/metrics`：
   - 状态：已完成。
   - 已实现低基数 HTTP counter、延迟 histogram、创建 / 跳转 / 缓存 / 后端错误 counter。
   - 已实现可配置的 `/metrics` 和 Prometheus 文本输出。
   - 已覆盖并发更新、内存模式、MySQL / Redis hit / miss / backfill 和 Redis 不可用 fallback 验证。
4. v1.5.3 Prometheus、Grafana 与 dashboard：
   - 状态：已完成。
   - 已在本地 Compose 中接入固定版本的 Prometheus 和 Grafana。
   - 已实现自动数据源、六面板 dashboard、监控数据卷和端到端冒烟验证。
5. v1.5.4 自动化验证、性能回归和文档收口：
   - 状态：尚未实现。
