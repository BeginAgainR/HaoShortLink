# 数据模型

状态：已建立基础版，持续维护
当前实现：已实现短链生命周期记录、MySQL 持久化、可选 Redis 生命周期查询缓存和 v1.9 访问统计投影

## 说明

本文档用于记录短链接业务的数据概念和持久化方向。当前实现支持进程内存保存短码到原始 URL
的映射，也支持 MySQL 持久化和可选 Redis 查询缓存。

当前代码已经将短链接存储抽象为 repository 接口，默认实现仍是内存版。

## 概念实体

### ShortLink

表示一条短链接映射关系。

候选字段：

- `id`：数据库自增主键，MySQL 版用于生成 Base62 短码。
- `code`：短码。
- `original_url`：原始 URL。
- `created_at`：创建时间。
- `updated_at`：更新时间。
- `expires_at`：UTC 过期时间，可空；v1.7 已实现。
- `status`：`active` 或 `disabled`；v1.7 已实现。

v1.1 计划先只持久化短链映射本身，不引入用户归属、访问统计、过期策略或软删除状态。

### Visit

表示一次短链接访问记录。

候选字段：

- `code`：访问的短码。
- `visited_at`：访问时间。
- `ip`：访问来源 IP，后置能力。
- `client_info`：客户端信息，后置能力。

当前状态：访问统计不属于 V1，也不属于 v1.1。

### User

表示用户或链接所有者。

当前状态：用户系统暂缓，不属于 V1，也不属于 v1.1。

## v1.1 持久化模型

### MySQL 表

v1.1 新增 `short_links` 表，作为短链映射的事实来源。

当前表结构：

```sql
CREATE TABLE IF NOT EXISTS short_links (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    code VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL,
    original_url TEXT NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_short_links_code (code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

说明：

- `id` 由 MySQL 自增生成，用于生成 Base62 短码。
- `code` 保存最终短码，必须唯一；创建事务中允许临时为 `NULL`，用于先获取自增 id 再回写短码。
- `original_url` 保存原始 URL，v1.1 不额外拆分域名或路径字段。
- `created_at` 和 `updated_at` 用于基础审计和后续排查。
- 暂不增加 `user_id`、`expires_at`、`status`、访问次数等字段。
- `code` 显式使用 `utf8mb4_bin` 排序规则，使 MySQL 唯一索引与 Base62 短码的大小写敏感语义一致。

### 短码生成

v1.1 使用：

```text
MySQL AUTO_INCREMENT id -> Base62(code)
```

创建短链时先让 MySQL 生成自增 id，再将 id 转换为 Base62 短码并在同一事务中写回
`code` 字段。这样服务重启后不会因为进程内计数器重置而重复生成已有短码。

## v1.7 生命周期迁移

v1.7 通过独立迁移为现有表增加：

```sql
status VARCHAR(16) NOT NULL DEFAULT 'active'
expires_at DATETIME NULL
```

已有记录自动保持 `active` 且 `expires_at IS NULL`，即迁移不会改变已有短链的跳转结果。
`expires_at` 按 UTC 解释；过期由读取时比较当前时间计算，不持久化 `expired` 状态。

### Redis 缓存

v1.1 中 Redis 只缓存短码跳转查询结果：

```text
shortlink:{code} -> original_url
```

查询语义：

- Redis 命中时可直接返回跳转。
- Redis 未命中时必须继续查询 MySQL。
- MySQL 命中后回填 Redis。
- Redis 不可用不应直接导致短链不存在，MySQL 仍是事实来源。

当前状态：MySQL 持久化和 Redis 查询缓存均已接入；Redis 默认关闭，需要通过配置显式启用。

## v1.9 访问统计投影

迁移 `004_create_access_statistics.sql` 新增：

- `processed_access_events`：以 `event_id` 为主键保存来源坐标和处理 disposition，是重放幂等边界。
- `short_link_access_totals`：以 `(short_link_id, result)` 为主键保存累计次数及最早 / 最近事件时间。
- `short_link_access_hourly`：以 `(short_link_id, bucket_start_epoch, result)` 为主键保存 UTC 小时桶；天趋势查询时汇总小时数据。

统计表通过 `short_link_id` 外键关联 `short_links.id`。`not_found` 只保存 ignored 收据，不按任意短码创建统计行；
无法关联的其他结果作为 orphan 进入 DLQ。收据当前不自动清理，因为在仍允许 Kafka retained events 重放时
删除收据会破坏幂等边界。

## 待决策

- 是否允许同一个原始 URL 生成多个短码。
- 是否需要为 `original_url` 增加长度限制或 hash 索引。
- 是否需要为不同环境设置不同的 Redis 默认 TTL。
