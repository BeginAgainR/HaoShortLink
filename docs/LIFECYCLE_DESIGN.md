# 链接生命周期设计

状态：v1.7 已实现并完成本地回归与 GitHub Actions 云端 CI

## 目标

v1.7 在现有创建、跳转、MySQL 持久化和 Redis 查询缓存之上补齐短链接自身的基础生命周期：

- 支持有效和禁用状态。
- 支持可选过期时间。
- 禁用或过期链接不再跳转。
- 提供仅供可信内部环境使用的详情、列表和生命周期修改接口。
- 明确旧数据迁移、旧缓存兼容和缓存失效语义。

## 数据语义

`short_links` 新增：

- `status`：`active` 或 `disabled`，非空，默认 `active`。
- `expires_at`：UTC 时间，可空；空值表示永不过期。

已有数据迁移后保持 `active` 且永不过期。过期是一种由 `expires_at` 和当前时间计算出的结果，
不新增 `expired` 持久化状态，也不依赖后台定时任务。

创建和更新请求中的时间使用严格的 RFC 3339 UTC 秒级格式：

```text
YYYY-MM-DDTHH:MM:SSZ
```

创建时不接受已经到期的时间。内部更新允许将 `expires_at` 设为 `null` 以恢复为永不过期，
也允许设置过去时间使链接立即过期。

## 跳转语义

跳转查询区分 `success`、`not_found`、`disabled` 和 `expired`。只有 `active` 且未过期的记录返回
`302 Found`。

公开的 `GET /s/{code}` 对不存在、禁用和过期统一返回：

```text
404 Not Found
```

这样不会通过公开响应暴露短码是否曾经存在。日志和 Prometheus 指标仍保留具体业务结果。

## 内部接口边界

v1.7 提供：

```text
GET /internal/short-links/{code}
GET /internal/short-links?limit=<1..100>&cursor=<id>&status=<active|disabled>
PUT /internal/short-links/{code}
```

`PUT` 只允许修改 `status` 和 `expires_at`，不能修改短码、原始 URL 或创建时间。列表按自增 `id`
正序使用 cursor 分页，默认 50 条，最多 100 条。

这些接口不是完整的认证或授权方案。v1.7 依靠部署边界保护它们：Nginx 不代理 `/internal/`，
Compose 中应用直连端口只绑定到 `127.0.0.1`。因此它们只适用于本机运维或可信内部调用。
用户身份、链接归属、RBAC 和正式管理 API 留到 v2.0。

## Redis 缓存

Redis 仍是可选查询缓存，MySQL 仍是事实来源。缓存值从纯 `original_url` 升级为带版本的完整生命周期记录，
包含状态、过期时间和原始 URL。

- 旧版纯 URL 缓存不再可信，读取时按 miss 处理并删除。
- 缓存命中后仍检查状态和过期时间。
- 有过期时间时，Redis TTL 不得晚于业务过期时间。
- 查询缓存配置 TTL 必须在 1 到 86400 秒之间，非法值回退为 3600 秒，避免出现永久陈旧缓存。
- 生命周期修改成功后同步删除对应缓存键。
- Redis 不可用时读取回退到 MySQL，修改仍以 MySQL 结果为准。
- MySQL 与 Redis 不使用分布式事务；异常或极端并发下的陈旧窗口由缓存 TTL 限制，并记录错误指标和日志。

## 不在 v1.7 范围内

- 用户、链接归属、登录、RBAC 和公网管理后台。
- 硬删除、批量操作、恢复历史和定时清理任务。
- 访问事件、访问统计、Kafka 或其他新基础设施。
- 自定义域名、二维码和复杂 URL 风控。
- 为生命周期功能大规模改写 `HttpServer/` 或数据库连接池。

## 验证要求

- 旧表应用迁移后数据仍可跳转。
- 永不过期、未来过期、已经过期和禁用语义正确。
- 详情、分页、状态过滤和生命周期更新正确。
- 生命周期更新后热缓存失效。
- 旧格式陈旧缓存不能绕过生命周期判断，生命周期修改会使已有新版缓存失效。
- Redis 不可用时 MySQL 读取和生命周期更新仍可工作。
- Linux VM 构建、CTest、API smoke 和 MySQL / Redis 集成测试通过。
