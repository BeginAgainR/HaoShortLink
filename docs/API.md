# API 设计

状态：已建立基础版，持续维护
当前实现：已实现短链接核心接口，支持内存存储、MySQL 持久化、可选 Redis 查询缓存、内部访问统计和可配置的 Prometheus 指标入口

## 路径命名原则

- 面向程序调用的接口放在 `/api/` 下。
- 面向用户直接访问和分享的短链跳转路径保持简短。
- 使用名词表示资源，使用 HTTP 方法表示动作。
- 不提前设计 V1 不需要的接口。

## V1 接口

V1 接口已实现。默认存储为进程内存；v1.1 增加 MySQL 持久化和可选 Redis 查询缓存，不改变对外 API。

### 健康检查

当前状态：已实现。

```text
GET /api/health
```

用途：

确认服务是否可访问。

响应示例：

状态码：

```text
200 OK
```

响应体：

```json
{
  "status": "ok"
}
```

v1.6 保留该兼容入口，并已增加：

```text
GET /api/health/live
GET /api/health/ready
```

- liveness 只表示进程和 HTTP 事件循环能够响应，不同步探测外部依赖。
- readiness 在 MySQL 存储模式下以 MySQL 为必要依赖，MySQL 不可用时返回 `503 Service Unavailable`。
- Redis 查询缓存和 Redis 限流为可降级依赖，不单独将 readiness 改为失败。
- 详细边界见 `docs/RELIABILITY_DESIGN.md`。

### 创建短链接

当前状态：已实现。

```text
POST /api/short-links
```

用途：

提交原始 URL，创建一个短链接。

请求头：

```text
Content-Type: application/json
```

请求示例：

```json
{
  "url": "https://example.com/very/long/path",
  "expires_at": "2026-08-01T00:00:00Z"
}
```

V1 URL 校验规则：

- `url` 字段必须存在。
- `url` 字段必须是字符串。
- `url` 字段必须以 `http://` 或 `https://` 开头。
- `expires_at` 可省略；省略或为 `null` 表示永不过期。
- `expires_at` 使用 `YYYY-MM-DDTHH:MM:SSZ` UTC 格式，创建时必须晚于当前时间。
- 不在 V1 中实现域名解析、黑名单或复杂安全策略。

成功状态码：

```text
201 Created
```

响应示例：

```json
{
  "code": "000001",
  "short_url": "/s/000001",
  "original_url": "https://example.com/very/long/path"
}
```

错误场景：

- 请求体不是合法 JSON：`400 Bad Request`。
- 缺少 `url` 字段：`400 Bad Request`。
- `url` 不是字符串或不符合 V1 最小 URL 规则：`400 Bad Request`。

v1.6 已对该创建接口增加可配置的全局 Redis 固定窗口限流。超限时返回：

```text
429 Too Many Requests
Retry-After: <seconds>
```

```json
{
  "error": {
    "code": "rate_limit_exceeded",
    "message": "Too many requests"
  }
}
```

限流默认关闭。Redis 限流故障时采用 fail-open，创建请求继续执行并记录日志和指标。

### 短码跳转

当前状态：已实现。

```text
GET /s/{code}
```

用途：

根据短码查找原始 URL，并返回重定向响应。

成功状态码：

```text
302 Found
```

行为：

```text
302 Found
Location: https://example.com/very/long/path
```

错误场景：

- 短码不存在：`404 Not Found`。
- 短码已禁用：`404 Not Found`。
- 短码已过期：`404 Not Found`。

三种不可跳转结果对公网使用相同响应，内部日志和指标仍分别记录实际结果。

## v1.7 内部生命周期接口

当前状态：已实现并完成本地回归。

```text
GET /internal/short-links/{code}
GET /internal/short-links?limit=<1..100>&cursor=<id>&status=<active|disabled>
PUT /internal/short-links/{code}
```

详情和列表返回 `id`、`code`、`original_url`、`status`、`expires_at`、`created_at` 和
`updated_at`。列表按 `id` 正序分页，并返回下一页 cursor。

更新请求只允许提供 `status` 和 `expires_at`：

```json
{
  "status": "disabled",
  "expires_at": null
}
```

这组接口不经过 Nginx 公网入口，也不构成完整认证方案；只允许从应用所在主机或可信内部环境调用。
用户身份、链接归属和正式权限控制留到 v2.0。

## v1.9 内部访问统计接口

当前状态：已实现；本地完整 Compose 回归和 GitHub Actions 云端 CI 均已通过。

```text
GET /internal/short-links/{code}/statistics
```

查询参数：

- `interval=hour|day`，默认 `day`。
- `from` 包含、`to` 不包含，使用 `YYYY-MM-DDTHH:MM:SSZ`。
- 显式时间必须与所选 UTC 小时或 UTC 天边界对齐。
- 小时范围最多 31 天，天范围最多 366 天；默认返回对齐后的最近 7 天。

响应中的 `summary` 是全量累计，`trend` 只覆盖请求范围。`access_count` 只统计成功跳转，
`attempt_count` 包含可关联到该短链的 `success`、`disabled`、`expired` 和 `error` 事件。

```json
{
  "code": "000001",
  "consistency": "eventual",
  "summary": {
    "access_count": 12,
    "attempt_count": 15,
    "result_counts": {
      "success": 12,
      "disabled": 1,
      "expired": 2,
      "error": 0
    },
    "last_access_at": "2026-07-18T12:00:00.123Z",
    "last_attempt_at": "2026-07-18T12:00:00.123Z"
  },
  "trend": {
    "interval": "day",
    "from": "2026-07-12T00:00:00Z",
    "to": "2026-07-19T00:00:00Z",
    "points": []
  }
}
```

不存在短链返回 `404`；存在但尚无统计时返回零 summary 和空 trend。接口只在
`storage.type=mysql` 且 `statistics.enabled=true` 时注册，并继续被默认 Nginx `/internal/` 规则阻断。

## 可观测性接口

### Prometheus 指标

当前状态：已实现进程内指标与文本暴露；本地 Compose 已接入 Prometheus 抓取和 Grafana dashboard。

```text
GET /metrics
```

用途：

以 Prometheus text exposition format 返回 HTTP 请求量、状态分类、延迟 histogram，以及短链创建、跳转、Redis 缓存和后端错误计数。

配置：

```text
metrics.enabled=true
```

第一版默认启用；设置为 `false` 时不注册该路由，直连请求返回 `404 Not Found`。

成功响应：

```text
200 OK
Content-Type: text/plain; version=0.0.4; charset=utf-8
```

该入口用于服务内部采集。Nginx 默认不转发 `/metrics`，不应作为公开业务 API 暴露。

## 错误响应

状态：暂定

框架层默认错误响应使用 JSON 格式：

```json
{
  "error": {
    "code": "not_found",
    "message": "Not Found"
  }
}
```

V1 业务错误示例：

```json
{
  "error": {
    "code": "invalid_request",
    "message": "Invalid request"
  }
}
```

```json
{
  "error": {
    "code": "short_link_not_found",
    "message": "Short link not found"
  }
}
```

## 待补充

- API 是否引入版本号，例如 `/api/v1`。
