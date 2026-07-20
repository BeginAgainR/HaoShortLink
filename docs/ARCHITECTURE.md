# 架构说明

状态：已建立基础版，持续维护
当前实现：框架层之上已实现短链闭环、MySQL / Redis、生命周期、流量保护、可观测性、可选 Kafka 访问事件和 MySQL 访问统计投影

## 分层

```text
apps/shortlink_server/      短链接业务层
apps/shortlink_event_consumer/ 独立访问事件消费者
HttpServer/                 HTTP 框架层
muduo                       网络库
```

`HttpServer/` 是当前已有的框架层，负责 HTTP 请求解析、路由、中间件、响应构造等基础能力。
`apps/shortlink_server/` 是业务应用层，承载短链接 API、业务逻辑、存储抽象、MySQL 存储、Redis 查询缓存和
Kafka publisher 和内部统计查询。`apps/shortlink_event_consumer/` 是独立进程，负责事件校验、MySQL 幂等统计
投影、DLQ、重试和 lag 观测；它只复用业务层的版本化事件 codec，不进入 HTTP 进程。

当前框架层已补充：

- 请求日志。
- 统一 JSON 错误响应。
- JSON 响应辅助。
- `key=value` 配置加载工具。

## 请求流

```text
muduo TcpServer
  -> HttpServer
  -> HttpContext
  -> Router / Middleware
  -> Handler
  -> HttpResponse
```

该请求流描述当前框架的目标结构，后续实现短链接业务时应尽量复用现有框架能力。

当前请求流：

```text
GET /api/health
  -> health handler
  -> JSON 200

POST /api/short-links
  -> create handler
  -> ShortLinkService
  -> ShortLinkRepository
  -> JSON 201

GET /s/{code}
  -> RedirectHandler
  -> ShortLinkService
  -> ShortLinkRepository
  -> Memory / MySQL / Redis cache + MySQL
  -> 302 / 404 / 500
  -> AccessEventPublisher（Noop 或 librdkafka 异步 fail-open）
```

当前本地工程化链路：

```text
client
  -> Nginx
  -> shortlink_server
       -> MySQL（事实来源）
       -> Redis（可选查询缓存）
       -> Kafka access-events（可选、非 readiness 依赖）
            -> shortlink_event_consumer（校验、MySQL 幂等投影、手动 offset）
                 -> access-events DLQ（永久非法或不兼容事件）
                 -> consumer /health 与 /metrics
       -> GET /internal/short-links/{code}/statistics（异步统计查询）
```

## 边界约定

- 不重写整个框架。
- 暂时不重命名 `HttpServer/`。
- 业务代码应放在 `apps/shortlink_server/`。
- Kafka 类型和访问事件逻辑不进入 `HttpServer/`、repository 或 `ShortLinkService`。
- 框架增强应保持小步修改，并避免引入短链接业务耦合。
- MySQL 是事实来源；Redis 和 Kafka 都是可降级依赖，Kafka 故障不改变跳转与 readiness 语义。
- v1.8 只建立事件管道；v1.9 在业务层增加统计副作用、`event_id` 幂等、DLQ 和受控重放。
- v1.9 只保证已进入 Kafka 且仍在 retention 范围内的有效事件不会因 consumer 重试或重启重复累计，
  不承诺 producer 到 MySQL 的端到端 exactly-once。

## 后续关注点

- v2.0 补用户、链接归属、对象级权限、自定义短码、管理 API、最小管理页面和 OpenAPI；同时拆分应用层
  过大的入口与组合职责，不重写 `HttpServer/`。
- v2.1 使用 Kubernetes 验证应用工作负载、配置与凭据、健康探针、多副本、滚动发布、故障恢复和可重复演示；
  第一版不声明生产级有状态集群能力。
- v2.2 对本来就写 MySQL 的链接生命周期操作使用 Transactional Outbox，由独立 relay 发布，并由生命周期
  consumer 形成可查询审计投影；访问跳转继续保持异步 fail-open 事件边界。
- v2.2 完成全链路、迁移、Kubernetes、性能、长稳和干净环境终验后作为阶段性功能冻结点。
- Schema Registry、RabbitMQ、Go、搜索 / 分析引擎和云基础设施只在出现真实需求后评估，不属于阶段终点。
