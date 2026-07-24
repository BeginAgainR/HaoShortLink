# 数据模型

状态：v2.0 已实现

MySQL 是持久化模式下用户、会话、短链接和访问统计的事实存储。Redis 只保存可丢弃的短码查询缓存与
限流窗口；Kafka 保存有限 retention 范围内的访问事件。

## Schema 版本

顺序 SQL 位于 `apps/shortlink_server/sql/`：

| 版本 | 作用 |
| --- | --- |
| 001 | 创建 `short_links` |
| 002 | 固化短码大小写敏感排序规则 |
| 003 | 增加状态和过期时间 |
| 004 | 增加访问事件收据与统计投影 |
| 005 | 增加用户、会话、owner 和 `schema_migrations` |

`schema_migrations(version, name, applied_at)` 记录已应用版本。迁移脚本支持空库初始化和 v1.9 原地升级，
看到版本 5 时仍会校验用户、会话、owner、索引、外键、历史版本和 `legacy-system` 是否完整。

## `users`

| 字段 | 语义 |
| --- | --- |
| `id` | 自增用户 id |
| `username` | 注册时的展示形式，最大 32 字符 |
| `username_normalized` | ASCII 小写规范化值，唯一且大小写不敏感登录 |
| `password_hash` | 版本化的 scrypt 编码串 |
| `status` | `active` 或 `disabled` |
| `created_at` / `updated_at` | 基础审计时间 |

密码格式为：

```text
scrypt$32768$8$1$<16-byte-salt-hex>$<32-byte-derived-key-hex>
```

每个密码使用独立随机 salt，校验使用常量时间比较。数据库不保存明文或可逆密码。

迁移 005 创建一个保留用户：

```text
username_normalized=legacy-system
status=disabled
password_hash=!
```

该账号不能登录，只承接 v1.9 历史链接归属。

## `user_sessions`

| 字段 | 语义 |
| --- | --- |
| `token_hash` | 原始 32 字节随机 token 的 SHA-256 十六进制摘要，主键 |
| `user_id` | 会话所属用户，外键到 `users.id` |
| `expires_at` | 固定绝对过期时间 |
| `revoked_at` | 显式退出时间，可空 |
| `created_at` | 创建时间 |

客户端 Cookie 只持有原始 token，数据库和日志不保存原始值。认证查询同时要求：会话存在、未撤销、
未过期且用户仍为 `active`。当前没有 refresh token、滑动续期或自动清理作业；过期和已撤销行会保留，
后续如数据量需要再增加与会话最长 TTL 对齐的受控清理任务。

## `short_links`

| 字段 | 语义 |
| --- | --- |
| `id` | 自增主键；随机短码的 Base62 发号来源 |
| `owner_id` | 非空 owner，外键到 `users.id` |
| `code` | 全局唯一、大小写敏感短码，最大 32 字符 |
| `original_url` | 原始 HTTP(S) URL |
| `status` | `active` 或 `disabled` |
| `expires_at` | 可空 UTC 时间；空表示永不过期 |
| `created_at` / `updated_at` | 创建和更新时间 |

关键索引：

- `uk_short_links_code(code)`：短码并发唯一性最终裁决。
- `idx_short_links_owner_id_id(owner_id, id)`：owner 范围 cursor 分页。
- `idx_short_links_owner_status_id(owner_id, status, id)`：owner + 状态分页。

公开管理查询在 SQL 中同时使用 `code` 和 `owner_id`，不会先读取任意 owner 的对象再只在响应层过滤。
公共跳转只按全局 `code` 查询，不要求登录。

过期不是持久化状态；服务在读取时通过 `expires_at <= now` 计算。这样不会让 `status` 和时间形成两个
相互冲突的事实来源。

## 访问统计投影

迁移 004 创建：

- `processed_access_events`：以 `event_id` 为主键保存 Kafka 来源坐标和 disposition，是消费幂等边界。
- `short_link_access_totals`：按 `(short_link_id, result)` 保存累计次数和首末事件时间。
- `short_link_access_hourly`：按 `(short_link_id, bucket_start_epoch, result)` 保存 UTC 小时桶；天趋势按小时汇总。

`not_found` 事件没有可关联的 `short_link_id`，只保存 ignored 收据；其他无法关联的事件进入 DLQ。
收据不自动清理，因为 Kafka retained events 仍可重放时删除收据会破坏幂等边界。

统计查询先以当前用户和短码确认 owner，再读取按 `short_link_id` 关联的投影。统计是最终一致的，不是短链
事实数据。

## Redis 查询缓存

缓存键：

```text
shortlink:{code}
```

v2 缓存值包含 `id`、`owner_id`、状态、过期时间、审计时间和原始 URL。旧版纯 URL 或损坏值会被删除并
按 miss 回源 MySQL。缓存 TTL 不超过配置 TTL，也不会晚于业务过期时间；生命周期更新后同步失效缓存。

Redis 不可用、miss 或无效缓存都不能改变 MySQL 事实语义。

## 回滚边界

`sql/rollback/005_remove_users_sessions_and_ownership.sql` 只回滚 v2.0 新增的用户、会话、owner 和版本 5
记录，保留 v1.9 的短链接生命周期与统计数据。执行入口要求显式传入 `--allow-data-loss`，因为用户和会话
数据会被删除。生产回滚仍应先备份并停止写入；脚本不是无停机双向迁移方案。
