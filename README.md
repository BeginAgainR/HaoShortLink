# HaoShortLink

HaoShortLink 是一个基于 muduo 网络库的 C++17 HTTP 框架项目，当前目标是在现有
`HttpServer/` 框架层之上，逐步构建短链接后端服务，并以该业务为载体引入常见后端工程能力。

## 当前状态

- `HttpServer/` 是现有可复用 HTTP 框架层，暂时保持目录名不变。
- `apps/shortlink_server/` 是短链接业务服务目录，已实现健康检查、创建短链接和短码跳转。
- v1.1 已接入 MySQL 持久化和可选 Redis 查询缓存。
- v1.2 工程化运行已完成本地 Docker Compose 验证，可启动 MySQL、Redis、`shortlink_server` 和 Nginx。
- v1.3 已完成第一版测试与 CI 收口，包含第一批 CTest、API 冒烟测试、MySQL / Redis 集成测试脚本和 CI 第一版 workflow；GitHub Actions 云端 CI 已通过。
- v1.4 已完成性能与稳定性收口，建立 curl / `hey` 多模式基线，覆盖关键异常场景并验证 Nginx 入口；本地结果不作为生产承载承诺。
- v1.5 已完成可观测性收口，覆盖 request ID、通用结构化请求日志、基础 HTTP / 短链指标、`/metrics`、本地 Prometheus / Grafana、最小 dashboard 和监控链路 CI 冒烟验证。
- v1.6 已完成 Redis Lua 全局创建限流、fail-open、liveness / readiness、保护性测试和 GitHub Actions 云端 CI 收口。
- v1.7 已完成链接状态、过期时间、内部详情 / 列表 / 更新接口、生命周期缓存失效、本地全量回归和 GitHub Actions 云端 CI 验证。
- v1.8 已完成版本化访问事件、librdkafka 异步 fail-open producer、独立 consumer、Kafka KRaft / Kafka UI Compose overlay、故障测试和 GitHub Actions 云端 Kafka CI 验证。
- v1.9 已完成本地实现、完整 Compose 回归和独立干净目录验证：MySQL 幂等统计投影、内部统计查询、重试 / DLQ、consumer 健康与 lag、受控重放和隔离重建；分支提交后的 GitHub Actions 结果仍待确认。
- MySQL / Redis 依赖集成和 Prometheus / Grafana 监控冒烟已进入 CI；代表性 `hey` 小基线已完成 v1.4.0 / v1.5 相对回归，本地结果不作为生产承载承诺。
- 旧五子棋业务代码已经清理，旧图片资源已移除。
- 已完成请求日志、统一 JSON 错误响应、JSON 响应辅助和配置加载等框架基础能力。
- 构建验证在 Linux 虚拟机或容器环境中进行，不在 Mac 宿主机上构建。

## 项目结构

```text
HttpServer/                 现有 HTTP 框架层
apps/shortlink_server/      短链接业务服务目录
apps/shortlink_event_consumer/ 独立访问事件消费者
deploy/nginx/               Nginx 本地反向代理配置
deploy/prometheus/          Prometheus 本地抓取配置
deploy/grafana/             Grafana 数据源和 dashboard provisioning
docs/                       公开项目文档
tests/                      自动化测试和测试脚本
```

## 文档

- [项目概览](docs/PROJECT_OVERVIEW.md)
- [架构说明](docs/ARCHITECTURE.md)
- [路线图](docs/ROADMAP.md)
- [决策记录](docs/DECISIONS.md)
- [已知问题](docs/BUGS.md)
- [短链接需求](docs/SHORTLINK_REQUIREMENTS.md)
- [API 设计](docs/API.md)
- [数据模型](docs/DATA_MODEL.md)
- [中间件设计](docs/MIDDLEWARE_DESIGN.md)
- [可观测性设计](docs/OBSERVABILITY_DESIGN.md)
- [可靠性与流量保护设计](docs/RELIABILITY_DESIGN.md)
- [链接生命周期设计](docs/LIFECYCLE_DESIGN.md)
- [访问事件与 Kafka 设计](docs/ACCESS_EVENT_DESIGN.md)
- [访问统计设计](docs/ACCESS_STATISTICS_DESIGN.md)
- [运行手册](docs/RUNBOOK.md)
- [测试计划](docs/TEST_PLAN.md)
- [压测计划](docs/BENCHMARK.md)
- [部署计划](docs/DEPLOYMENT.md)

## 说明

当前短链接服务支持内存存储、MySQL 持久化、可选 Redis 查询缓存、可选 Redis 全局创建限流、链接生命周期、可选 Kafka 访问事件和 MySQL 访问统计投影。v1.9 统计 API 保持内部边界，统计异步可见；producer fail-open 和 7 天 topic retention 仍决定事件完整性与可重建范围。
未实现内容会在 `docs/` 中以“草案”“计划”或“暂缓”的形式记录，避免将未来能力描述为已完成能力。
