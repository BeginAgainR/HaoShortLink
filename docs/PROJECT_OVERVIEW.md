# 项目概览

状态：草案
当前实现：已实现健康检查、创建短链接、短码跳转、MySQL 持久化、可选 Redis 查询缓存、本地 Docker Compose 运行方式、第一批自动化测试和 CI 第一版 workflow

## 项目定位

HaoShortLink 是一个基于 muduo 网络库的 C++17 HTTP 框架项目。当前目标是在保持
`HttpServer/` 框架层稳定的基础上，在上层逐步构建短链接后端服务。

## 当前状态

- 旧五子棋业务代码已经清理。
- `HttpServer/` 保留为现有 HTTP 框架层，暂时不重命名。
- `apps/shortlink_server/` 是新的短链接业务服务目录。
- `apps/shortlink_server/` 已实现健康检查、创建短链接和短码跳转。
- 短链接存储支持内存版和 MySQL 版。
- MySQL 版支持可选 Redis 查询缓存。
- 已完成本地 Docker Compose 验证，可启动 MySQL、Redis、`shortlink_server` 和 Nginx。
- 已新增第一批 CTest、API 冒烟测试、MySQL / Redis 集成测试脚本和 CI 第一版 workflow。
- CI 第一版核心命令链路已在 Linux VM 中验证，GitHub Actions 云端 CI 已通过。
- 完整部署方案尚未实现。
- 已完成请求日志、统一 JSON 错误响应、JSON 响应辅助和配置加载等框架基础能力。
- 下一阶段重点是性能稳定性和可观测性建设。

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

docs/                       公开项目文档
deploy/nginx/               Nginx 本地反向代理配置
tests/                      自动化测试和测试脚本
.github/workflows/          GitHub Actions workflow
```

## 当前不包含的能力

以下能力尚未实现，不应在公开文档中描述为已完成：

- Redis 限流
- 压测
- 指标监控
- 消息队列
- 告警策略

## 文档维护原则

- README 只作为项目入口和文档导航。
- 详细设计、路线、问题和运行说明放在 `docs/`。
- 未实现能力使用“草案”“计划”“暂缓”“尚未实现”等状态标注。
- 文档应随着项目实现和验证结果持续更新。
