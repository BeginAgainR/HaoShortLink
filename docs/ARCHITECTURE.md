# 架构说明

状态：已建立基础版，持续维护
当前实现：框架层之上已实现短链闭环、MySQL / Redis、生命周期、流量保护、可观测性和可选 Kafka 访问事件

## 分层

```text
apps/shortlink_server/      短链接业务层
apps/shortlink_event_consumer/ 独立访问事件消费者
HttpServer/                 HTTP 框架层
muduo                       网络库
```

`HttpServer/` 是当前已有的框架层，负责 HTTP 请求解析、路由、中间件、响应构造等基础能力。
`apps/shortlink_server/` 是业务应用层，承载短链接 API、业务逻辑、存储抽象、MySQL 存储、Redis 查询缓存和
Kafka publisher。`apps/shortlink_event_consumer/` 是独立进程，只复用版本化事件 codec，不进入 HTTP 进程。

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
            -> shortlink_event_consumer（校验、记录、手动 offset）
```

## 边界约定

- 不重写整个框架。
- 暂时不重命名 `HttpServer/`。
- 业务代码应放在 `apps/shortlink_server/`。
- Kafka 类型和访问事件逻辑不进入 `HttpServer/`、repository 或 `ShortLinkService`。
- 框架增强应保持小步修改，并避免引入短链接业务耦合。
- MySQL 是事实来源；Redis 和 Kafka 都是可降级依赖，Kafka 故障不改变跳转与 readiness 语义。
- v1.8 只建立事件管道，不写访问统计，不承诺端到端 exactly-once。

## 后续关注点

- v1.9 将访问事件写入统计模型，并补充 `event_id` 幂等、失败处理、lag 和受控重放。
- v2.0 补用户、链接归属、权限和管理 API。
- 只有出现可靠生命周期事件需求后才评估 Outbox；只有出现多语言消费者或频繁 schema 演进后才评估
  Schema Registry、Avro 或 Protobuf。
