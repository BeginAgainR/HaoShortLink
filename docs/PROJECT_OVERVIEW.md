# 项目概览

状态：已建立基础版，持续维护
当前实现：已实现短链闭环、MySQL / Redis、生命周期、流量保护、可观测性、Kafka 访问事件管道和 MySQL 访问统计投影；v1.9 本地全量回归和 GitHub Actions 云端 CI 已通过

## 项目定位

HaoShortLink 是一个基于 muduo 网络库的 C++17 HTTP 框架项目。当前目标是在保持
`HttpServer/` 框架层稳定的基础上，在上层逐步构建短链接后端服务，并以短链接业务为载体
引入持久化、缓存、反向代理、测试、压测、可观测性、限流和消息队列等常见后端工程能力。

## 当前状态

- 旧五子棋业务代码已经清理。
- `HttpServer/` 保留为现有 HTTP 框架层，暂时不重命名。
- `apps/shortlink_server/` 是新的短链接业务服务目录。
- `apps/shortlink_server/` 已实现健康检查、创建短链接和短码跳转。
- 短链接存储支持内存版和 MySQL 版。
- MySQL 版支持可选 Redis 查询缓存。
- 已完成本地 Docker Compose 验证，可启动 MySQL、Redis、`shortlink_server` 和 Nginx。
- v1.3 已完成第一版测试与 CI 收口，包含第一批 CTest、API 冒烟测试、MySQL / Redis 集成测试脚本和 CI 第一版 workflow。
- CI 第一版核心命令链路已在 Linux VM 中验证，GitHub Actions 云端 CI 已通过。
- v1.4 已完成 curl / `hey` 多模式基线、关键异常场景、Redis 环境问题定性和 Nginx 入口底线验证。
- v1.5 已完成 request ID、结构化请求日志、HTTP / 短链指标、Prometheus、Grafana、六面板 dashboard 和监控链路 CI 冒烟验证。
- v1.6 已实现可配置的 Redis Lua 全局创建限流、fail-open、liveness / readiness 和保护性测试；GitHub Actions 云端 CI 已通过。
- v1.7 已实现 `active` / `disabled` 状态、可选过期时间、内部详情 / 分页 / 更新接口、Redis 生命周期缓存和失效测试；本地全量回归和 GitHub Actions 云端 CI 已通过。
- v1.8 已实现 Kafka 访问事件、异步 fail-open producer、独立 consumer、本地 KRaft / Kafka UI 编排、观测和故障验证。
- v1.9 已实现 MySQL 幂等统计投影、内部统计查询、重试 / DLQ、consumer 健康与 lag、受控重放和隔离重建；本地全量回归和 GitHub Actions 云端 CI 已通过。
- 完整部署方案尚未实现。
- 已完成请求日志、统一 JSON 错误响应、JSON 响应辅助和配置加载等框架基础能力。
- MySQL / Redis 依赖集成、监控冒烟、限流和健康语义测试已配置进入 CI。

## 目录结构

```text
HttpServer/
  include/                  框架头文件
  src/                      框架实现
  examples/                 框架示例或测试客户端

apps/
  shortlink_server/
    include/                短链接业务头文件预留目录
    src/                    短链接服务入口和业务实现目录
    config/                 配置文件预留目录
    sql/                    数据库脚本预留目录
  shortlink_event_consumer/
    include/                consumer 配置、统计写入、DLQ 和观测组件
    src/                    独立 Kafka consumer 入口
    config/                 consumer 配置样例

docs/                       公开项目文档
deploy/nginx/               Nginx 本地反向代理配置
deploy/prometheus/          Prometheus 本地抓取配置
deploy/grafana/             Grafana 数据源和 dashboard provisioning
tests/                      自动化测试和测试脚本
.github/workflows/          GitHub Actions workflow
```

## 当前不包含的能力

以下能力尚未实现，不应在公开文档中描述为已完成：

- 生产告警和长期容量规划
- 公开统计 API、独立访客、地理位置或 Referer 等分析维度
- 用户系统
- 后台管理
- 告警策略

v1.8 事件边界见 `docs/ACCESS_EVENT_DESIGN.md`；v1.9 统计语义、幂等、DLQ 和重放边界见
`docs/ACCESS_STATISTICS_DESIGN.md`。

## 文档维护原则

- README 只作为项目入口和文档导航。
- 详细设计、路线、问题和运行说明放在 `docs/`。
- 未实现能力使用“草案”“计划”“暂缓”“尚未实现”等状态标注。
- 文档应随着项目实现和验证结果持续更新。
