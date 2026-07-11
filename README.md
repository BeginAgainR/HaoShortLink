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
- 下一阶段为 v1.5 可观测性与依赖 CI，先补结构化日志、基础指标和 MySQL / Redis 远端集成验证。
- 旧五子棋业务代码已经清理，旧图片资源已移除。
- 已完成请求日志、统一 JSON 错误响应、JSON 响应辅助和配置加载等框架基础能力。
- 构建验证在 Linux 虚拟机或容器环境中进行，不在 Mac 宿主机上构建。

## 项目结构

```text
HttpServer/                 现有 HTTP 框架层
apps/shortlink_server/      短链接业务服务目录
deploy/nginx/               Nginx 本地反向代理配置
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
- [运行手册](docs/RUNBOOK.md)
- [测试计划](docs/TEST_PLAN.md)
- [压测计划](docs/BENCHMARK.md)
- [部署计划](docs/DEPLOYMENT.md)

## 说明

当前短链接服务支持内存存储、MySQL 持久化和可选 Redis 查询缓存；已提供并验证本地 Docker Compose 编排，可启动 MySQL、Redis、`shortlink_server` 和 Nginx；v1.3 已完成第一版自动化测试和 CI 收口，v1.4 已完成性能与稳定性基线。消息队列和监控能力尚未实现。
未实现内容会在 `docs/` 中以“草案”“计划”或“暂缓”的形式记录，避免将未来能力描述为已完成能力。
