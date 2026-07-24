# v2.0 产品闭环设计

状态：v2.0 已实现并完成本地验收
目标版本：v2.0.5

## 目标

v2.0 在现有短链生命周期和访问统计之上补齐可直接使用的产品闭环：用户可以注册、登录、创建并管理
自己的短链接，通过同源管理页面查看生命周期和访问统计；公共访问者无需登录即可跳转。

该版本保持 `HttpServer/` 为通用框架层，身份、会话、对象授权和页面均属于
`apps/shortlink_server/` 应用层。

## 范围

v2.0 必须交付：

- 用户名和密码注册、登录、退出、当前用户查询。
- 服务端持久化会话、固定过期、显式撤销和账号禁用后的会话失效。
- 所有新建短链接都有 owner。
- 用户只能查询、修改和查看自己链接的统计。
- 随机短码与自定义短码创建。
- 登录、创建、列表、生命周期管理和统计查看的同源管理页面。
- 与实际路由一致的 OpenAPI 文档。
- 空库初始化、v1.9 原地升级、历史链接回填和迁移回滚验证。

v2.0 不实现：

- 平台管理员、跨用户管理、RBAC 或运营后台。
- 邮箱、验证码、找回密码、OIDC、邀请和计费。
- 硬删除、链接转移、自定义域名或多租户配额。
- React、Vue 等独立前端工程和 Node.js 构建链。

## 身份模型

用户使用唯一 `username` 和密码注册。用户名对登录和唯一约束采用小写规范化值，响应保留用户注册时的
展示值。第一版用户名规则为：

- 3 到 32 个字符。
- 首字符为 ASCII 字母。
- 后续字符只允许 ASCII 字母、数字、下划线和连字符。
- 大小写不构成不同账号。

用户状态为 `active` 或 `disabled`。禁用用户不能登录；已存在会话在认证时视为无效。

v1.9 升级时创建保留的禁用账号 `legacy-system`，已有短链接全部归属该账号。历史链接继续允许公共跳转，
但不出现在普通用户管理页面，也不能被普通用户认领。该规则避免第一个注册用户意外获得已有数据。

## 密码与会话

- 密码只保存专用密码哈希格式，不保存明文、可逆密文或日志副本。
- 登录成功后生成密码学安全的随机 token。
- 客户端只通过 `HttpOnly` Cookie 持有原始 token，数据库只保存 token 摘要。
- Cookie 使用 `Path=/`、`HttpOnly` 和 `SameSite=Lax`；HTTPS 部署必须启用 `Secure`。
- 会话采用固定绝对过期时间，第一版默认 7 天，不实现滑动续期或 refresh token。
- 登录创建新会话；退出撤销当前会话；认证时同时检查会话未撤销、未过期且用户仍为 active。
- 登录、退出和其他状态修改接口校验同源请求。日志、错误响应和指标不得包含密码、Cookie 或 token。
- 登录失败统一返回相同错误，不区分账号不存在、账号被禁用或密码错误。

现有 `HttpServer/session/` 的内存会话不满足上述持久化、摘要、撤销和多副本边界，v2.0 不直接复用；
Header、Cookie 和响应头等通用能力继续复用现有框架。

## 授权模型

v2.0 只有一种可登录主体：普通用户。对象授权规则为：

```text
authenticated_user.id == short_link.owner_id
```

对象不存在和无权访问统一返回 `404`，避免通过状态码枚举其他用户的短码。认证缺失或会话无效返回 `401`。
公共 `GET /s/{code}` 不执行登录检查，继续将不存在、禁用和过期统一表现为公网 `404`。

owner 判断集中在应用层授权服务和 owner-aware repository 查询中，不在各 handler 中重复散落。数据查询应把
`owner_id` 放入 SQL 条件，不能先读取任意对象再只依赖响应层过滤。

## API 契约

项目版本号不作为 URL 版本号。v2.0 继续使用 `/api`，不并行维护 `/api/v2`。

### 公共接口

```text
GET    /api/health
GET    /api/health/live
GET    /api/health/ready
POST   /api/auth/register
POST   /api/auth/login
GET    /s/{code}
```

注册和登录成功均设置会话 Cookie。开放注册由 `auth.registration_enabled` 控制；Compose 演示配置开启。

### 登录接口

```text
DELETE /api/auth/session
GET    /api/me
POST   /api/short-links
GET    /api/short-links?limit=<1..100>&cursor=<id>&status=<active|disabled>
GET    /api/short-links/{code}
PUT    /api/short-links/{code}
GET    /api/short-links/{code}/statistics
```

`POST /api/short-links` 从 v2.0 起必须登录。现有匿名创建行为不保留；公共跳转行为保持不变。

创建请求：

```json
{
  "url": "https://example.com/path",
  "expires_at": "2026-08-01T00:00:00Z",
  "custom_code": "docs_2026"
}
```

`expires_at` 和 `custom_code` 均可省略或为 `null`。响应继续返回 `code`、`short_url`、`original_url`、
`status`、`expires_at`、`created_at` 和 `updated_at`。

### 错误语义

| 状态 | 错误码 | 说明 |
| --- | --- | --- |
| 400 | `invalid_request` | JSON、字段类型或字段集合不合法 |
| 400 | `invalid_username` | 用户名不符合规则 |
| 400 | `invalid_password` | 密码不符合最小规则 |
| 400 | `invalid_custom_code` | 自定义短码不符合规则或属于保留字 |
| 401 | `authentication_required` | 没有有效会话 |
| 403 | `registration_disabled` | 当前部署关闭注册 |
| 403 | `origin_not_allowed` | 浏览器状态修改请求不是同源请求 |
| 404 | `short_link_not_found` | 对象不存在或不属于当前用户 |
| 409 | `username_conflict` | 用户名已经存在 |
| 409 | `short_code_conflict` | 自定义短码已经存在 |
| 429 | `rate_limit_exceeded` | 命中既定流量保护 |

## 自定义短码

- 字符集：ASCII 字母、数字、下划线和连字符。
- 长度：4 到 32 个字符。
- 比较与唯一约束：大小写敏感，与当前 `utf8mb4_bin` 数据库语义一致。
- 保留字：`api`、`app`、`health`、`internal`、`metrics`、`s`。
- 冲突：依赖数据库唯一约束作为并发最终裁决，返回 `409 short_code_conflict`。
- 自定义短码同样归属于创建者；不同用户不能复用已经存在的短码。
- 同一个原始 URL 可以创建多条随机或自定义短链。

## 管理页面

管理页面位于 `/app/`，由 Nginx 以同源静态资源提供，不引入独立前端构建链。最小页面包含：

- 注册和登录。
- 当前用户和退出。
- 随机或自定义短码创建。
- owner 范围内的分页列表。
- 禁用、恢复和过期时间修改。
- 单条链接访问统计和异步一致性提示。
- 401、输入错误、冲突、依赖故障和空列表反馈。

页面不提供用户管理、历史系统链接认领或跨用户查询。

## 数据迁移

迁移使用顺序版本和 `schema_migrations` 记录。迁移入口与 HTTP 服务进程分离，避免未来多个 Pod 同时把
应用启动当作迁移锁。Compose 和测试在启动服务前显式执行迁移。

v2.0 数据模型至少增加：

- `users`
- `user_sessions`
- `short_links.owner_id`
- owner 分页索引
- `schema_migrations`

旧的 `001` 到 `004` SQL 继续保留，不原地重写历史迁移。升级测试必须分别从空库和真实 v1.9 schema
执行，验证历史短链跳转、统计外键和生命周期数据不变。回滚只回滚 v2.0 新增对象和 owner 字段，不删除
v1.9 业务数据。

## 应用层边界

`main.cpp` 只负责信号、配置、依赖组装和启动。v2.0 将职责拆为：

```text
ServerConfig
  -> application composition
  -> repositories / services
  -> route registration
  -> auth, short-link, statistics and health handlers
```

身份、owner、页面或 OpenAPI 类型不得进入 `HttpServer/`。只有出现可独立复用且现有框架确实缺失的通用
HTTP 能力时，才以独立小改动增强框架并补框架测试。

## 验收重点

- 两个用户之间的列表、详情、修改和统计越权全部被拒绝且不泄露对象存在性。
- Cookie、密码和 token 不出现在结构化请求日志、错误响应或指标 label 中。
- 重复用户名和重复自定义短码在并发下只有一个请求成功。
- 退出、过期和禁用账号的旧会话立即失效。
- Redis 或 Kafka 故障不改变既定短链跳转语义；MySQL 仍是身份和 owner 的事实存储。
- v1.9 API、生命周期、统计、Kafka fail-open、限流、健康和监控回归继续通过。
