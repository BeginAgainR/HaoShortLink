# 可观测性设计

状态：v1.5 第一版设计基线
当前实现：尚未接入结构化日志、应用指标、Prometheus 或 Grafana

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
- 如果客户端提供合法的 `X-Request-ID`，是否沿用需要限制长度和字符范围；不合法时由服务重新生成。
- 服务生成或接受的 request ID 应写入响应头 `X-Request-ID`。
- 同一请求的框架日志、业务日志和错误日志使用相同 request ID。
- request ID 只用于日志关联，不作为 Prometheus label。
- request ID 生成失败不能导致业务请求崩溃；实现前需要确定无额外依赖的生成策略。

## 结构化日志

第一版优先使用稳定的 key-value 或 JSON 结构，不依赖终端展示文本做自动解析。

### 通用请求日志字段

| 字段 | 含义 |
| --- | --- |
| `timestamp` | 记录时间 |
| `level` | 日志级别 |
| `event` | 稳定事件名，例如 `http_request` |
| `request_id` | 请求关联标识 |
| `method` | HTTP 方法 |
| `route` | 路由模板，例如 `/s/:code`，不记录原始高基数路径 |
| `status` | HTTP 状态码 |
| `duration_ms` | 请求处理耗时 |
| `error_code` | 可选的稳定错误码 |

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

第一版指标名称在实现前允许小幅调整，但语义和标签边界应保持稳定。

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

请求延迟使用 histogram，第一版 bucket 需要覆盖当前亚毫秒到秒级的本地基线。具体 bucket 在实现批次中通过现有 benchmark 数据确定，并记录单位。

## `/metrics` 暴露

第一版约束：

- `GET /metrics` 返回 Prometheus text exposition format。
- 指标入口应可通过配置启用或关闭。
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

## Prometheus 与 Grafana

本地 Compose 计划增加：

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
   - 状态：第一版设计基线已建立。
   - Review 日志字段、request ID、指标、标签和暴露边界。
2. v1.5.1 request ID 与结构化日志：
   - 状态：尚未实现。
3. v1.5.2 进程内指标与 `/metrics`：
   - 状态：尚未实现。
4. v1.5.3 Prometheus、Grafana 与 dashboard：
   - 状态：尚未实现。
5. v1.5.4 自动化验证、性能回归和文档收口：
   - 状态：尚未实现。
