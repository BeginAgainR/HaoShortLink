# 项目概览

状态：v2.0 产品闭环已实现，等待最终发布分支 CI 结果

## 项目定位

HaoShortLink 是一个基于 muduo 的 C++17 HTTP 框架项目。`HttpServer/` 提供请求解析、路由、
中间件和响应等通用能力；`apps/shortlink_server/` 在其上实现可直接使用的短链接服务。

项目重点不是不断增加技术栈，而是让已经引入的 MySQL、Redis、Kafka、Nginx、Prometheus 和 Grafana
具有明确职责、故障边界和可重复验证证据。

## v2.0 产品闭环

普通用户可以：

1. 使用用户名和密码注册或登录。
2. 创建随机短码或自定义短码。
3. 只查看自己的短链接列表和详情。
4. 禁用、恢复或修改过期时间。
5. 查看由 Kafka 访问事件异步生成的统计。

公共访问者无需登录即可访问 `GET /s/{code}`。不存在、禁用和过期链接对公网统一返回 `404`。

v2.0 只有普通用户，不实现平台管理员、跨用户管理或 RBAC。“管理 API / 管理页面”指普通用户对自己
链接的对象级管理。v1.9 历史链接归属禁用的 `legacy-system` 账号，不允许普通用户自动认领。

## 当前架构能力

- MySQL 保存用户、会话摘要、链接、owner 和访问统计，是持久化模式的事实存储。
- Redis 是可选查询缓存和可选全局创建限流；故障时按既定回源 / fail-open 语义降级。
- Kafka producer 对跳转路径保持异步 fail-open；consumer 通过 `event_id` 和 MySQL 事务形成幂等统计投影。
- Nginx 提供同源 `/app/` 页面、`/api/` 和 `/s/` 入口，并阻断 `/internal/` 与 `/metrics`。
- Prometheus / Grafana 提供请求、缓存、后端错误、限流和事件链路观测。
- Compose 使用独立一次性 migration 服务，不把 schema 变更绑定到 HTTP 进程启动。

## 应用结构

```text
apps/shortlink_server/
  include/shortlink/       配置、认证、repository、service 和 HTTP API 接口
  src/                     应用实现与依赖组装
  sql/                     001-005 顺序迁移和 v2.0 回滚脚本
  config/                  本地、容器和 Kafka 配置样例
  web/                     无构建链的同源管理页面

apps/shortlink_event_consumer/
  include/                 consumer、统计写入、DLQ 和观测组件
  src/                     独立消费进程
  config/                  consumer 配置样例

HttpServer/                保持业务无关的 HTTP 框架层
deploy/                    本地入口与监控配置
tests/                     CTest、API、依赖、故障、迁移和契约验证
```

`main.cpp` 只负责信号处理、配置读取、依赖组装和启动；认证与 HTTP handler 已拆入应用层组件。
v2.0 没有修改 `HttpServer/`。

## 已验证边界

- 密码使用 OpenSSL scrypt；数据库不保存明文密码或原始会话 token。
- 会话固定过期、可撤销，账号禁用后旧会话失效；浏览器状态修改请求执行同源检查。
- owner 条件进入 repository / SQL 查询；对象不存在和跨 owner 访问统一返回 `404`。
- 用户名和自定义短码并发冲突由数据库唯一约束裁决。
- 空库初始化、v1.9 原地升级、历史 owner 回填、重复迁移和显式回滚已覆盖。
- v1.9 的 Redis 降级、健康语义、监控、Kafka fail-open、统计幂等、DLQ 和重放边界保持有效。

详细证据见 [v2.0 验收记录](V2_ACCEPTANCE.md)。

## 当前不包含

- 平台管理员、用户运营后台、链接转移或跨用户查询。
- 邮箱、验证码、密码找回、OIDC、邀请、计费和自定义域名。
- HTTPS/TLS 终止和生产发布方案。
- Kubernetes 多副本验收；该范围属于 v2.1。
- Transactional Outbox 和生命周期审计投影；该范围属于 v2.2。
- 生产级 MySQL、Redis 或 Kafka 集群运维和 SLA 承诺。
