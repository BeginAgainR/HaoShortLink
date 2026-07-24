# 部署边界

状态：v2.0 本地 Compose 方案已实现；线上部署与 Kubernetes 尚未实现

## 当前本地拓扑

`compose.yaml` 提供可重复的本地产品与监控环境：

```text
Nginx :8080
  -> /app/、/openapi.yaml
  -> shortlink_server
       -> MySQL
       -> Redis
       -> 可选 Kafka overlay
  -> Prometheus :9090 -> Grafana :3000
```

服务包括：

- `mysql`：用户、会话、链接 owner 和统计事实存储。
- `schema_migrate`：一次性迁移进程；成功退出后应用才启动。
- `redis`：可选查询缓存和可选全局创建限流。
- `shortlink_server`：HTTP 应用。
- `nginx`：同源管理页面与业务入口。
- `prometheus` / `grafana`：本地观测。

`compose.kafka.yaml` 额外提供单节点 KRaft Kafka、topic init、Kafka UI 和访问统计 consumer。该拓扑用于
开发和故障验收，不声明生产高可用。

## 启动与入口

```bash
docker compose up -d --build
```

端口：

| 入口 | 绑定 | 用途 |
| --- | --- | --- |
| Nginx | `0.0.0.0:8080` | `/app/`、OpenAPI、`/api/`、`/s/` |
| shortlink_server | `127.0.0.1:18080` | 本机调试、内部指标 |
| MySQL | `0.0.0.0:13306` | 本地开发依赖 |
| Redis | `0.0.0.0:16379` | 本地开发依赖 |
| Prometheus | `127.0.0.1:9090` | 本地指标查询 |
| Grafana | `127.0.0.1:3000` | 本地 dashboard |
| Kafka UI（overlay） | `127.0.0.1:18081` | 本地 topic / consumer 检查 |

Nginx 显式阻断 `/internal/` 和 `/metrics`。旧内部生命周期 / 统计路由已被有 Cookie 和 owner 授权的
`/api/short-links...` 取代，即使绕过 Nginx 直连也不再存在。

## 迁移与启动顺序

空 MySQL 数据卷会按文件名执行 001-005；已有数据卷由 `schema_migrate` 检测并升级。应用依赖迁移进程
成功退出：

```bash
docker compose run --rm schema_migrate
```

手工环境使用：

```bash
HAOHTTP_MYSQL_HOST=127.0.0.1 \
HAOHTTP_MYSQL_PORT=3306 \
bash tests/scripts/migrate_shortlink_schema.sh up
```

回滚 005 会删除用户和会话，必须先停止写入、完成备份并显式确认：

```bash
bash tests/scripts/migrate_shortlink_schema.sh down --allow-data-loss
```

回滚保留 v1.9 链接生命周期和统计表，但旧版本不具备 v2.0 用户管理能力。

## 健康与依赖

- 容器 healthcheck 使用 `/api/health/ready`。
- MySQL 是持久化模式的必要依赖；不可用时 readiness 为 `503`。
- Redis 查询缓存和限流可降级，不单独导致 readiness 失败。
- Kafka producer 和统计 consumer 不在 HTTP readiness 依赖链中。
- migration 失败时应用不会启动，避免在不完整 schema 上提供服务。

## 凭据和 HTTPS

Compose 内的 MySQL、Grafana 凭据只用于本地演示，不是生产默认值。真实部署必须通过受控 Secret 注入，
不能提交真实密码。

当前 Nginx 配置没有 HTTPS/TLS 终止。HTTPS 部署必须：

- 在可信入口终止 TLS 并传递正确 `Host` 与 `X-Forwarded-Proto`。
- 设置 `auth.cookie_secure=true`。
- 只公开入口代理，隐藏应用、数据库、缓存、Kafka、指标和管理依赖端口。

## 当前不声明

- 完整线上部署、集中日志、告警值班、备份恢复或 SLA。
- TLS 证书自动化和可信代理链配置。
- 生产级 MySQL、Redis、Kafka 或 Prometheus 高可用。
- Kubernetes、滚动发布、回滚和应用多副本；属于 v2.1。

## v2.1 计划

v2.1 将保留 Compose 作为轻量本地入口，同时增加固定一种本地 Kubernetes 环境，验证
`shortlink_server`、事件 consumer 和管理入口的 Deployment、Service、ConfigMap、Secret、探针、
资源限制、多副本、滚动发布、回滚和故障恢复。第一版不承担生产级有状态集群运维。
