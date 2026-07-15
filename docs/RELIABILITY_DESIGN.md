# 可靠性与流量保护设计

状态：v1.6.0 设计已收口，v1.6.1-v1.6.6 实现、本地端到端与干净克隆验证已完成，云端 CI 待确认

## 目标

v1.6 在不改变短链接核心 API 语义的前提下，补充基础流量保护、依赖降级和健康检查语义。

本阶段解决两类问题：

- 限制创建短链接路径对 MySQL 和服务容量的冲击。
- 区分进程存活、业务就绪和可降级依赖故障。

## 范围

v1.6 只对以下接口实施限流：

```text
POST /api/short-links
```

不限制以下路径：

- `GET /s/{code}`
- 健康检查
- `/metrics`

第一版使用全局创建额度，所有实例通过 Redis 共享同一个窗口计数。当前框架没有建立可靠的
客户端 IP 与可信代理边界，v1.6 不直接信任客户端可伪造的 `X-Forwarded-For` 或 `X-Real-IP`。
按 IP、用户或 API key 的滥用保护需在客户端身份边界明确后单独扩展。

## 限流算法

v1.6 使用 Redis Lua 固定窗口：

1. 对当前窗口 key 执行 `INCR`。
2. 计数第一次出现时设置过期时间。
3. 同一段 Lua 返回当前计数和剩余 TTL，避免 `INCR` 与 `EXPIRE` 之间的竞态。
4. 计数不超过阈值时放行，超过阈值时拒绝。

固定窗口的边界突刺是已知权衡。v1.6 优先获得小而可验证的原子分布式限流，不提前引入滑动窗口或令牌桶复杂度。

## 配置

已增加以下配置：

```text
rate_limit.enabled=false
rate_limit.requests=100
rate_limit.window_seconds=60
rate_limit.key_prefix=rate-limit:create:
```

边界：

- 默认关闭，升级后不自动改变现有请求行为。
- `requests` 和 `window_seconds` 必须为正整数。
- Redis 连接参数复用现有 `redis.host`、`redis.port` 和 `redis.database`。
- `rate_limit.enabled` 与查询缓存的 `redis.enabled` 相互独立。内存存储或关闭 Redis 查询缓存时仍可开启限流。
- 密钥前缀不应和短链查询缓存前缀重叠。

## HTTP 语义

超限请求返回：

```text
429 Too Many Requests
Retry-After: <seconds>
Content-Type: application/json
```

```json
{
  "error": {
    "code": "rate_limit_exceeded",
    "message": "Too many requests"
  }
}
```

限流检查在创建请求的 Content-Type、JSON 和 URL 校验之前执行，使非法创建流量同样消耗额度。
被限流的请求不进入 `ShortLinkService` 或 repository。

## Redis 故障降级

Redis 限流采用 fail-open：

- Redis 连接、`SELECT`、Lua 执行或回复解析失败时，当前请求继续执行。
- 故障必须写入结构化日志和低基数指标。
- Redis 故障不得将创建请求转换为 429 或 500。
- 限流不作为 MySQL 错误或容量问题的遮盖手段。

选择 fail-open 的原因是 Redis 在当前系统中不是业务事实存储。当 Redis 故障时，服务宁可短时失去流量保护，
也不应将所有创建流量拒绝为不可用。

## 健康检查

v1.6 已提供：

```text
GET /api/health
GET /api/health/live
GET /api/health/ready
```

`/api/health` 保留兼容，语义与 liveness 相同。

### Liveness

- 进程和 HTTP 事件循环能够处理请求时返回 `200 OK`。
- 不对 MySQL 或 Redis 执行同步探测。
- 依赖故障不应诱导运行环境重启仍然存活的进程。

### Readiness

- 内存存储模式在服务完成初始化后返回 `200 OK`。
- MySQL 存储模式只在 MySQL 有界探测成功时返回 `200 OK`，失败时返回 `503 Service Unavailable`。
- Redis 查询缓存或 Redis 限流失败不改变 readiness，因为它们都有明确的 fail-open / 回源语义。
- readiness 不得因等待连接池或依赖超时而无限阻塞。

当前响应：

```json
{
  "status": "ready"
}
```

```json
{
  "status": "not_ready"
}
```

## 可观测性

限流第一版增加低基数计数器：

```text
haohttp_shortlink_rate_limit_checks_total{result="allowed|limited|error"}
```

约束：

- IP、URL、短码、Redis key 和 `request_id` 不能作为指标标签。
- 429 同时进入通用 HTTP 指标的 `4xx` 分类。
- Redis 限流错误记录后端 `redis` / 操作 `rate_limit` 的错误计数。
- 结构化日志区分 `allowed`、`limited` 和 `error` / `fail_open`。

## 验证范围

v1.6 至少覆盖：

- 限流关闭时的现有行为兼容。
- 窗口内放行、超限、过期恢复和并发原子性。
- Redis 不可用时 fail-open。
- 内存存储 + Redis 限流。
- MySQL 存储、关闭查询缓存 + Redis 限流。
- liveness / readiness 在内存、MySQL 正常和 MySQL 故障下的语义。
- Nginx 入口下的 429、`Retry-After` 和健康检查。
- 限流日志、指标与通用 HTTP 指标。

## 非目标

- 按 IP、用户、API key 或短码限流。
- 可信代理、CIDR 和客户端真实 IP 解析。
- 限制短码跳转路径。
- 滑动窗口、令牌桶、Nginx `limit_req` 或多级限流。
- Redis 连接池重构。
- 自动封禁、黑名单、风控和身份系统。
- 生产告警、容量承诺或完整线上发布体系。
