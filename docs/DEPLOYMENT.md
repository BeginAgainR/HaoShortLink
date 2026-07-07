# 部署计划

状态：草案
当前实现：已整理 Linux VM 手工运行说明；已新增并验证 MySQL/Redis 本地 Docker Compose 依赖编排；Nginx 尚未接入

## 说明

本文档用于后续记录 HaoShortLink 的部署方式。当前项目已接入 MySQL 和可选 Redis 查询缓存，
并新增了本地开发用 MySQL/Redis Docker Compose 依赖编排。当前 Compose 依赖运行在
OrbStack Docker 中，`shortlink_server` 仍在 `haoHTTP` Linux VM 中构建和运行。当前尚未将 `shortlink_server`
容器化，也尚未接入 Nginx，因此本文档暂不提供完整容器化或线上部署步骤。

当前可用的运行方式记录在：

```text
docs/RUNBOOK.md
```

当前边界：

- 支持在 Linux VM 中手工构建和运行 `shortlink_server`。
- 支持使用内存存储、MySQL 持久化和可选 Redis 查询缓存。
- 支持使用 OrbStack Docker Compose 启动本地 MySQL 和 Redis 依赖。
- 尚未提供 `shortlink_server` 服务容器。
- 尚未提供 Nginx 反向代理入口。
- 尚未定义线上发布、回滚、日志采集或监控方案。

## 后续内容

计划补充：

- Linux 环境依赖。
- 构建方式。
- 服务启动方式。
- 配置文件说明。
- `shortlink_server` Dockerfile。
- Nginx 反向代理配置。
- MySQL 和 Redis 依赖说明。
- 日志和故障排查。

## v1.2 拆分建议

v1.2 建议继续按小步推进：

1. 整理运行配置和运行手册。
2. 增加 MySQL、Redis 的本地 Docker Compose 依赖编排。
3. 增加服务 Dockerfile。
4. 增加 Nginx 反向代理配置。
5. 更新部署验证记录。

## 当前不声明

当前不声明以下能力已经可用：

- 容器化部署。
- `shortlink_server` 服务容器。
- Nginx 接入。
- Redis 限流。
- 线上运行方案。
