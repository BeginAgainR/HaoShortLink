# HaoShortLink

HaoShortLink 是一个基于 muduo 网络库的 C++17 HTTP 框架与短链接服务项目。项目在保持
`HttpServer/` 通用框架层稳定的前提下，通过真实短链接业务验证持久化、缓存、权限、异步事件、
可观测性、故障恢复和工程化测试。

## 当前能力

- 用户名 / 密码注册和登录，服务端持久化 `HttpOnly` Cookie 会话。
- 用户只能创建、查询和修改自己的短链接；匿名访问者仍可使用短码跳转。
- 随机短码、自定义短码、大小写敏感唯一约束、禁用 / 恢复和过期时间。
- `/app/` 同源静态管理页面，覆盖登录、创建、列表、生命周期和访问统计。
- MySQL 事实存储、可选 Redis 查询缓存和可选 Redis 全局创建限流。
- 可选 Kafka 访问事件、独立 consumer、MySQL 幂等统计投影、重试、DLQ、lag 和受控重放。
- liveness / readiness、结构化请求日志、Prometheus 指标和 Grafana dashboard。
- Docker Compose、Nginx、OpenAPI、CTest、API / 依赖 / 故障集成测试和 GitHub Actions workflow。

v2.0 不包含平台管理员、跨用户管理、RBAC、邮箱找回、计费、自定义域名或复杂前端工程。管理页面是
普通用户管理自己链接的入口。Kubernetes 和 Transactional Outbox 分别属于后续 v2.1、v2.2。

## 快速体验

本地 Compose 会先执行 schema migration，再启动 MySQL、Redis、服务、Nginx、Prometheus 和 Grafana：

```bash
docker compose up -d --build
```

入口：

- 管理页面：`http://127.0.0.1:8080/app/`
- OpenAPI：`http://127.0.0.1:8080/openapi.yaml`
- 健康检查：`http://127.0.0.1:8080/api/health`
- Prometheus：`http://127.0.0.1:9090`
- Grafana：`http://127.0.0.1:3000`

完整启动、迁移、配置和故障处理步骤见 [运行手册](docs/RUNBOOK.md)。项目在 Linux VM 或 Linux
容器中构建和验证，不在 Mac 宿主机上生成构建产物。

## 项目结构

```text
HttpServer/                     通用 HTTP 框架层
apps/shortlink_server/          HTTP 应用、认证、短链业务与静态页面
apps/shortlink_event_consumer/  Kafka 访问事件消费者与统计投影
deploy/                         Nginx、Prometheus 和 Grafana 配置
docs/                           设计、契约、运行和验收文档
tests/                          单元、集成、故障和契约测试
```

## 文档

- [项目概览](docs/PROJECT_OVERVIEW.md)
- [v2.0 产品闭环设计](docs/PRODUCT_DESIGN.md)
- [API 说明](docs/API.md) / [OpenAPI 契约](docs/openapi.yaml)
- [架构说明](docs/ARCHITECTURE.md)
- [数据模型](docs/DATA_MODEL.md)
- [路线图](docs/ROADMAP.md)
- [决策记录](docs/DECISIONS.md)
- [运行手册](docs/RUNBOOK.md)
- [部署边界](docs/DEPLOYMENT.md)
- [测试计划](docs/TEST_PLAN.md)
- [v2.0 验收记录](docs/V2_ACCEPTANCE.md)
- [阶段性终点验收](docs/FINAL_ACCEPTANCE.md)

其余专题设计位于 `docs/`。未实现能力会明确标记为计划或非目标，不作为当前能力宣传。
