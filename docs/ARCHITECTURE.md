# 架构说明

状态：v2.0 已实现

## 分层

```text
Nginx
  -> /app/ 与 /openapi.yaml        静态产品入口
  -> /api/* 与 /s/*                shortlink_server

shortlink_server
  -> HttpServer                    通用 HTTP 框架
  -> AuthHttp / ShortLinkHttpApi   路由与协议适配
  -> AuthService / ShortLinkService
  -> AuthRepository / ShortLinkRepository / AccessStatisticsRepository
  -> MySQL                         用户、会话、链接、owner、统计事实
  -> Redis                         可选查询缓存与全局创建限流
  -> Kafka                         可选异步访问事件

shortlink_event_consumer
  -> Kafka consumer group
  -> MySQL 幂等统计投影
  -> Kafka DLQ
```

`HttpServer/` 只负责请求解析、路由、中间件、响应、通用日志与指标等 HTTP 能力。身份、Cookie、同源策略、
owner、自定义短码、页面和 OpenAPI 全部位于应用层；v2.0 没有修改框架目录。

## 应用进程职责

`apps/shortlink_server/src/main.cpp` 只负责：

- 读取 `ServerConfig`。
- 初始化 MySQL 连接池和可选 Redis / Kafka 组件。
- 组装 repository、service 和 HTTP API。
- 启动服务并处理有界优雅退出。

主要应用组件：

- `ServerConfig`：解析并校验服务、认证、存储、缓存、限流和 Kafka 配置。
- `AuthHttp`：Cookie、认证响应、注册 / 登录 / 退出 / 当前用户路由。
- `AuthService`：用户名规则、scrypt 密码校验、随机 token、摘要、固定过期和撤销。
- `SameOriginPolicy`：拒绝明确的跨站浏览器状态修改请求。
- `ShortLinkHttpApi`：健康、创建、owner 管理、统计和公共跳转路由。
- `ShortLinkService`：URL、短码、生命周期和时间规则。
- repository：内存或 MySQL 实现，以及 Redis cache-aside 装饰器。

## 身份与管理请求流

```text
browser / API client
  -> Nginx same-origin entry
  -> AuthHttp
       -> same-origin check（状态修改）
       -> hao_session Cookie -> SHA-256 digest
       -> AuthRepository -> user_sessions JOIN users
  -> ShortLinkHttpApi
       -> authenticated user id
       -> ShortLinkService
       -> owner-aware repository / SQL
  -> JSON response with Cache-Control: no-store
```

对象授权规则只有一个：

```text
authenticated_user.id == short_links.owner_id
```

列表、详情、更新和统计都把 owner 放入查询边界。不存在和跨 owner 访问统一返回 `404`。v2.0 不实现平台
管理员或特殊绕过角色，因此不存在第二套跨用户授权路径。

内存模式保留相同 API 和 owner 语义，适合快速测试；进程重启会丢失用户、会话和链接，不能用于多副本。

## 公共跳转与异步统计

```text
GET /s/{code}
  -> ShortLinkService
  -> Redis cache hit 或 MySQL 回源
  -> 302 / 404 / 500
  -> AccessEventPublisher
       -> Noop 或 librdkafka 异步 fail-open
       -> Kafka access-events
            -> shortlink_event_consumer
            -> event_id 幂等 MySQL 统计投影
            -> 永久非法事件 DLQ
```

公共跳转不读取登录会话。Redis 和 Kafka 都不是跳转正确性的事实来源：Redis 失败回源 MySQL；Kafka 初始化、
队列或 broker 失败只产生日志和指标，不改变 HTTP 结果或 readiness。

consumer 在 MySQL 事务完成或 DLQ delivery 确认后才提交源 offset。临时错误有界重试，达到上限后进程
非零退出并由编排恢复。`event_id` 唯一约束吸收重复；系统不声明跨 Kafka 与 MySQL 的端到端 exactly-once。

## 数据库迁移

迁移不是 HTTP 进程启动副作用：

```text
Compose / operator
  -> schema_migrate one-shot process
  -> migration 001..005 + schema validation
  -> shortlink_server / consumer start
```

这样避免未来多副本同时把应用启动当作迁移锁。空库与 v1.9 升级共用同一入口；显式 down 只回滚 005，
并要求调用者确认用户和会话数据丢失。

## 网络与暴露边界

- Nginx 公开 `/app/`、`/openapi.yaml`、`/api/` 和 `/s/`。
- Nginx 显式阻断 `/internal/` 和 `/metrics`。
- 应用直连端口在 Compose 中只绑定 `127.0.0.1`，用于本地调试和 Prometheus 抓取。
- Prometheus、Grafana 和 Kafka UI 只绑定本机地址。
- HTTPS 尚未在仓库中终止；部署到 HTTPS 时必须设置 `auth.cookie_secure=true`。

同源检查是 Cookie 状态修改的浏览器防护，不替代 TLS、可信反向代理配置或公网风控。

## 未来边界

- v2.1 在 MySQL 模式上验证 Kubernetes 工作负载、多副本、发布、回滚和故障恢复；不把内存模式用于扩容。
- v2.2 对本来就写 MySQL 的生命周期变更增加 Transactional Outbox、relay 和可查询审计投影。
- 访问跳转仍保持异步 fail-open，不为每次读取同步写 outbox。
- Go、RabbitMQ、Schema Registry、搜索 / 分析引擎和云基础设施只有出现真实需求后再评估。
