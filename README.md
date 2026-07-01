# HaoShortLink

HaoShortLink 是一个基于 muduo 网络库的 C++17 HTTP 框架项目，当前目标是在现有
`HttpServer/` 框架层之上，逐步构建短链接后端服务。

## 当前状态

- `HttpServer/` 是现有可复用 HTTP 框架层，暂时保持目录名不变。
- `apps/shortlink_server/` 是短链接业务服务目录，目前业务逻辑尚未实现。
- 旧五子棋业务代码已经清理，旧图片资源已移除。
- 已完成请求日志、统一 JSON 错误响应、JSON 响应辅助和配置加载等框架基础能力。
- 构建验证在 Linux 虚拟机或容器环境中进行，不在 Mac 宿主机上构建。

## 项目结构

```text
HttpServer/                 现有 HTTP 框架层
apps/shortlink_server/      短链接业务服务目录
docs/                       公开项目文档
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

当前短链接业务、数据库、缓存、容器化部署、Nginx、消息队列和监控能力均尚未实现。
相关内容会在 `docs/` 中以“草案”“计划”或“暂缓”的形式记录，避免将未来能力描述为已完成能力。
