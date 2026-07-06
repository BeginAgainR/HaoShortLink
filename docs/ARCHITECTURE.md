# 架构说明

状态：草案
当前实现：框架层存在，短链接业务层已实现 V1 最小闭环、MySQL 持久化和可选 Redis 查询缓存

## 分层

```text
apps/shortlink_server/      短链接业务层
HttpServer/                 HTTP 框架层
muduo                       网络库
```

`HttpServer/` 是当前已有的框架层，负责 HTTP 请求解析、路由、中间件、响应构造等基础能力。
`apps/shortlink_server/` 是业务应用层，当前承载短链接 API、业务逻辑、存储抽象、MySQL 存储实现和 Redis 查询缓存。

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
  -> redirect handler
  -> ShortLinkService
  -> ShortLinkRepository
  -> Memory / MySQL / Redis cache + MySQL
  -> 302 Location
```

## 边界约定

- 不重写整个框架。
- 暂时不重命名 `HttpServer/`。
- 业务代码应放在 `apps/shortlink_server/`。
- 框架增强应保持小步修改，并避免引入短链接业务耦合。
- 部署、限流和进阶能力在明确任务前不提前接入。

## 后续关注点

- Docker Compose、Nginx 和部署流程如何后续梳理。
- 框架测试如何覆盖路由、响应构造和错误响应。
- CORS、中间件执行顺序和限流能力如何后续梳理。
