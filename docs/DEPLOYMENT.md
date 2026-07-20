# 部署计划

状态：已建立本地方案，持续维护
当前实现：已整理 Linux VM 手工运行说明；已新增并验证本地 Docker Compose 编排；已接入 `shortlink_server`、Nginx、Prometheus、Grafana、服务内部 `/metrics` 和最小 dashboard

## 说明

本文档用于后续记录 HaoShortLink 的部署方式。当前项目已接入 MySQL 和可选 Redis 查询缓存，
并新增了本地开发用 Docker Compose 编排。当前 Compose 可以在 OrbStack Docker 中启动
MySQL、Redis、`shortlink_server`、Nginx、Prometheus 和 Grafana；也保留了在 `haoHTTP` Linux VM 中
手工构建和运行 `shortlink_server` 的调试方式。当前暂不提供完整线上部署步骤。

当前可用的运行方式记录在：

```text
docs/RUNBOOK.md
```

当前边界：

- 支持在 Linux VM 中手工构建和运行 `shortlink_server`。
- 支持使用内存存储、MySQL 持久化和可选 Redis 查询缓存。
- 支持使用 Redis 对创建短链接接口施加可选的全局固定窗口限流。
- Compose 使用 `/api/health/ready` 作为 `shortlink_server` 就绪检查，同时保留不探测依赖的 liveness。
- 支持使用 OrbStack Docker Compose 启动本地 MySQL、Redis、`shortlink_server`、Nginx、Prometheus 和 Grafana。
- 本地 Compose 保留只绑定 `127.0.0.1` 的 `shortlink_server:18080` 直连调试入口。
- `/metrics` 只通过 `shortlink_server` 内部或直连调试端口访问，Nginx 默认不转发。
- `/internal/` 生命周期管理接口只允许通过本机直连入口访问，Nginx 显式返回 `404`。
- Prometheus 和 Grafana 分别通过 `127.0.0.1:9090`、`127.0.0.1:3000` 提供本地调试入口，不绑定全部宿主机网卡。
- Prometheus 和 Grafana 使用 named volume；dashboard 和数据源通过仓库内 provisioning 文件自动加载。
- 生产化部署应只对外暴露 Nginx 的 80/443 入口，后端服务和数据库缓存只在内部网络可见。
- HTTPS/TLS 终止计划由 Nginx 配置承接，当前尚未实现。
- 尚未定义线上发布、回滚、集中日志、监控高可用或告警方案。

## 后续内容

计划补充：

- Linux 环境依赖。
- 构建方式。
- 服务启动方式。
- 配置文件说明。
- 生产化端口暴露策略。
- Nginx HTTPS/TLS 配置。
- MySQL 和 Redis 依赖说明。
- 日志和故障排查。

## v2.1 Kubernetes 计划

状态：草案，尚未实现。

第一版聚焦应用工作负载、多副本和可重复演示：

- 固定一种本地 Kubernetes 环境并记录版本和资源；不同时维护多套本地发行版。
- 部署 `shortlink_server`、`shortlink_event_consumer` 和管理入口，只在 MySQL 持久化模式下验证服务扩容。
- v2.2 再增加独立 outbox relay 和生命周期审计 consumer 工作负载。
- 使用 ConfigMap、Secret、Service、liveness / readiness probe 和资源 requests / limits。
- 在 Ingress 与复用现有 Nginx 之间选择受控入口，继续保护内部 API、`/metrics` 和依赖端口。
- 保持 Prometheus 可抓取，并验证滚动升级、回滚、Pod 重建、服务多副本和 Kafka consumer group 扩展。
- 提供明确的启动、健康检查、完整演示和清理入口，在独立干净环境执行成功。
- MySQL、Redis 和 Kafka 第一版允许使用外部依赖或仅供本地验收的单节点部署，不声明生产高可用。
- HPA、Helm 和生产级有状态集群运维在基础清单与负载证据稳定后再评估。

阶段终点需要同时保留 Compose 作为轻量本地开发入口和 Kubernetes 作为编排验收入口；Kubernetes
不是对 Compose 的删除式替换。详细终验见 `docs/FINAL_ACCEPTANCE.md`。

## v1.2 拆分建议

v1.2 建议继续按小步推进：

1. 整理运行配置和运行手册。
2. 增加 MySQL、Redis 的本地 Docker Compose 依赖编排。
3. 增加服务 Dockerfile。
4. 增加 Nginx 反向代理配置。
5. 更新部署验证记录。

## 当前不声明

当前不声明以下能力已经可用：

- 完整线上容器化部署。
- HTTPS/TLS 终止。
- 线上运行方案。
