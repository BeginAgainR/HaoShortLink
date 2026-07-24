# API 说明

状态：v2.0 已实现

机器可读契约以 [`docs/openapi.yaml`](openapi.yaml) 为准。Compose 环境通过
`http://127.0.0.1:8080/openapi.yaml` 提供同一文件，并由契约测试检查公开路由与服务注册路由是否漂移。

## 通用约定

- API 保持 `/api` 前缀，不为 v2.0 另建 `/api/v2`。
- JSON 请求必须使用 `Content-Type: application/json`。
- 时间使用秒级 UTC RFC 3339：`YYYY-MM-DDTHH:MM:SSZ`。
- 管理接口使用名为 `hao_session` 的 `HttpOnly` Cookie，不使用 JWT。
- 注册、登录、退出、创建和更新会校验浏览器同源请求；明确的跨站请求返回
  `403 origin_not_allowed`。没有 `Origin` 的非浏览器 API 客户端仍可调用。
- 认证缺失或会话无效返回 `401 authentication_required`。
- 对象不存在和不属于当前用户统一返回 `404 short_link_not_found`。
- 用户管理响应添加 `Cache-Control: no-store`；日志和指标不记录密码、Cookie 或 token。

## 路由总览

| 方法 | 路径 | 认证 | 用途 |
| --- | --- | --- | --- |
| GET | `/api/health` | 否 | 兼容健康检查 |
| GET | `/api/health/live` | 否 | 进程 liveness |
| GET | `/api/health/ready` | 否 | MySQL 模式 readiness |
| POST | `/api/auth/register` | 否 | 注册并创建会话 |
| POST | `/api/auth/login` | 否 | 登录并创建会话 |
| DELETE | `/api/auth/session` | Cookie 可选 | 幂等撤销当前会话 |
| GET | `/api/me` | 是 | 当前用户 |
| POST | `/api/short-links` | 是 | 创建随机或自定义短码 |
| GET | `/api/short-links` | 是 | owner 范围内分页列表 |
| GET | `/api/short-links/{code}` | 是 | owner 范围内详情 |
| PUT | `/api/short-links/{code}` | 是 | 修改状态或过期时间 |
| GET | `/api/short-links/{code}/statistics` | 是 | owner 范围内异步统计 |
| GET | `/s/{code}` | 否 | 公共短码跳转 |

`GET /metrics` 是内部 Prometheus 采集入口，不属于公开业务 OpenAPI；Nginx 默认返回 `404`。
旧 `/internal/short-links...` 管理路由在 v2.0 已移除，Nginx 和应用直连均不再提供。

## 身份与会话

### 注册

```http
POST /api/auth/register
Content-Type: application/json

{
  "username": "alice",
  "password": "correct-horse-battery"
}
```

用户名规则：

- 3 到 32 个 ASCII 字符。
- 首字符必须为字母，其余允许字母、数字、`_` 和 `-`。
- 唯一约束和登录不区分大小写，响应保留注册时的展示形式。

密码长度为 10 到 128 字节。成功返回 `201`、用户和固定会话过期时间，并设置 Cookie。开放注册由
`auth.registration_enabled` 控制；关闭时返回 `403 registration_disabled`。

重复用户名返回 `409 username_conflict`。

### 登录

```http
POST /api/auth/login
Content-Type: application/json

{
  "username": "Alice",
  "password": "correct-horse-battery"
}
```

成功返回 `200` 并创建新会话。账号不存在、密码错误和账号禁用统一返回：

```json
{
  "error": {
    "code": "invalid_credentials",
    "message": "Invalid username or password"
  }
}
```

### 当前用户与退出

```text
GET    /api/me
DELETE /api/auth/session
```

退出接口幂等：无论 Cookie 是否存在都返回 `204`，存在的当前会话会被撤销，客户端 Cookie 被清除。

Cookie 属性：

```text
Path=/; HttpOnly; SameSite=Lax; Max-Age=<固定剩余秒数>
```

HTTPS 部署必须设置 `auth.cookie_secure=true`，使响应同时包含 `Secure`。

## 创建短链接

```http
POST /api/short-links
Content-Type: application/json
Cookie: hao_session=...

{
  "url": "https://example.com/long/path",
  "expires_at": "2030-12-31T00:00:00Z",
  "custom_code": "launch_2026"
}
```

- `url` 必填，只接受 `http://` 或 `https://`。
- `expires_at` 可省略或为 `null`；创建时必须晚于当前时间。
- `custom_code` 可省略或为 `null`；省略时生成基于持久化 id 的 Base62 短码。
- 自定义短码长度 4 到 32，允许 ASCII 字母、数字、`_` 和 `-`，大小写敏感。
- `api`、`app`、`health`、`internal`、`metrics`、`s` 是保留短码。
- 全局短码唯一，不同用户不能复用已存在短码；并发冲突返回 `409 short_code_conflict`。

成功返回 `201`：

```json
{
  "id": 42,
  "code": "launch_2026",
  "short_url": "/s/launch_2026",
  "original_url": "https://example.com/long/path",
  "status": "active",
  "expires_at": "2030-12-31T00:00:00Z",
  "created_at": "2026-07-21T08:00:00Z",
  "updated_at": "2026-07-21T08:00:00Z"
}
```

创建接口继续支持可选 Redis 全局固定窗口限流。超限返回 `429`、`Retry-After` 和
`rate_limit_exceeded`；Redis 限流故障保持 fail-open。

## 查询和修改自己的链接

### 列表

```text
GET /api/short-links?limit=<1..100>&cursor=<id>&status=<active|disabled>
```

按自增 `id` 正序分页，默认 50 条。`cursor` 表示只返回 `id` 大于该值的记录。响应：

```json
{
  "items": [],
  "next_cursor": null
}
```

`next_cursor` 只有在可能存在下一页时才返回整数。所有查询都带当前用户 owner 条件。

### 详情

```text
GET /api/short-links/{code}
```

只返回当前用户的对象；其他用户的同一短码与不存在对象均返回相同 `404`。

### 生命周期更新

```http
PUT /api/short-links/{code}
Content-Type: application/json

{
  "status": "disabled",
  "expires_at": null
}
```

请求至少包含 `status` 或 `expires_at` 之一，且不能包含其他字段。`status` 只允许 `active` 或
`disabled`。更新允许把 `expires_at` 设置为过去时间，使链接立即过期；设置为 `null` 表示永不过期。

## 访问统计

```text
GET /api/short-links/{code}/statistics?interval=hour|day&from=<UTC>&to=<UTC>
```

- 仅在 `storage.type=mysql` 且 `statistics.enabled=true` 时注册。
- `from` 包含、`to` 不包含，并必须对齐到 UTC 小时或 UTC 天边界。
- 小时范围最多 31 天，天范围最多 366 天；默认返回最近 7 天。
- `summary` 是累计统计，`trend` 只覆盖请求范围。
- `access_count` 只统计成功跳转；`attempt_count` 包含可关联到该链接的 `success`、`disabled`、
  `expired` 和 `error`。
- `consistency` 固定为 `eventual`，因为统计来自 Kafka 异步投影。

存在但尚无统计的链接返回零 summary 和空 points。统计同样执行 owner 授权。

## 公共跳转

```text
GET /s/{code}
```

有效链接返回 `302 Found` 和目标 `Location`。不存在、禁用和过期对公网统一返回
`404 short_link_not_found`。跳转无需登录，Kafka 不可用不会改变 HTTP 结果或 readiness。

## 健康和内部观测

- `/api/health` 与 `/api/health/live` 只表示进程和 HTTP 事件循环可响应。
- `/api/health/ready` 在 MySQL 模式下探测 MySQL；必要依赖不可用时返回 `503`。
- Redis 查询缓存、Redis 限流、Kafka producer 和统计 consumer 不作为 HTTP readiness 必要依赖。
- `/metrics` 只通过应用直连或内部网络采集，不经过默认 Nginx 入口。

## 统一错误格式

```json
{
  "error": {
    "code": "invalid_request",
    "message": "Invalid request"
  }
}
```

v2.0 主要错误码：

| HTTP | 错误码 | 含义 |
| --- | --- | --- |
| 400 | `invalid_request` | JSON、字段类型或字段集合错误 |
| 400 | `invalid_username` / `invalid_password` | 凭据格式错误 |
| 400 | `invalid_custom_code` | 自定义短码格式或保留字错误 |
| 401 | `authentication_required` | 无有效会话 |
| 401 | `invalid_credentials` | 登录失败统一结果 |
| 403 | `registration_disabled` | 部署关闭开放注册 |
| 403 | `origin_not_allowed` | 明确的跨站状态修改请求 |
| 404 | `short_link_not_found` | 对象不存在、无权访问或不可跳转 |
| 409 | `username_conflict` | 用户名唯一约束冲突 |
| 409 | `short_code_conflict` | 短码唯一约束冲突 |
| 429 | `rate_limit_exceeded` | 创建接口达到全局额度 |
| 500 | `internal_server_error` / `authentication_error` | 必要持久化操作失败 |
