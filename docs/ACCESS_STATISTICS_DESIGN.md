# 访问统计设计

状态：v1.9 本地实现、完整故障回归和独立干净目录验证已完成，待云端 CI 最终确认

## 业务目标

v1.9 将 v1.8 的访问事件写成可查询的 MySQL 统计投影：

```text
GET /s/{code}
  -> Kafka hao-shortlink.access-events.v1
  -> shortlink_event_consumer
  -> MySQL 幂等统计投影
  -> GET /internal/short-links/{code}/statistics
```

“访问”仍表示短链接服务处理了一次跳转请求，不表示客户端最终成功访问目标站点。统计异步可见，
并继续接受 v1.8 producer fail-open 边界内已经明确的事件丢失可能；v1.9 只保证已经进入 Kafka 且仍在
retention 范围内的有效事件不会因为 consumer 重试或重启而重复累计。

## 交付边界

v1.9 实现：

- 已存在短链的成功访问次数、尝试次数、结果分类、最近访问时间和基础趋势。
- MySQL `event_id` 唯一约束、统计事务和处理后提交 Kafka offset。
- MySQL 临时失败退避重试，永久非法或不兼容事件写入独立 DLQ topic。
- consumer 处理、重复、重试、DLQ 和 lag 的低基数观测。
- Kafka retention 范围内的幂等重放，以及隔离环境中的统计重建验证。
- MySQL / Kafka / consumer 的本地、容器和测试配置分层与 CI。

v1.9 不实现：

- 用户、链接归属、权限、公开统计 API 或管理页面。
- IP、地理位置、Referer、User-Agent、独立访客、机器人识别或目标站点到达确认。
- 实时计费、事件绝不丢失、端到端 exactly-once 或生产级 SLA。
- 在线双投影、无停机统计重建或超出 Kafka retention 的历史恢复承诺。
- Redis 统计、Transactional Outbox、Schema Registry、Kafka 多 broker、TLS、SASL 或 ACL。
- 不把 Kafka、统计查询或消费副作用放入 `HttpServer/` 框架层。

## 统计语义

对已存在的短链提供以下字段：

- `access_count`：`result=success` 的累计次数。
- `attempt_count`：能够关联到该短链的全部有效事件次数。
- `result_counts`：`success`、`disabled`、`expired` 和 `error` 的分类次数。
- `last_access_at`：最近一次 `success` 的事件时间，没有成功访问时为 `null`。
- `last_attempt_at`：最近一次能够关联到该短链的事件时间。
- `trend`：按 UTC 小时或 UTC 天返回分类计数。

`not_found` 表示事件产生时短码不存在。随机扫描不存在短码可能形成无界高基数，因此 v1.9 不按这类
`code` 建立业务统计行；consumer 持久化其 `event_id` 处理收据并记录低基数 ignored 指标，使重放仍然幂等。
如果 `success`、`disabled`、`expired` 或 `error` 事件无法关联到 `short_links`，则视为语义异常并进入 DLQ。

事件时间使用 `occurred_at_ms`，而不是 consumer 处理时间。小时桶使用 UTC epoch 秒向下取整，避免 MySQL、
容器和宿主机时区不同导致聚合漂移。乱序消息使用 `LEAST` / `GREATEST` 更新最早和最近事件时间。

## 数据模型

v1.9 新增三个表：

### `processed_access_events`

保存最小持久化消费收据：

- `event_id`：32 位小写十六进制，主键和幂等边界。
- `source_topic`、`source_partition`、`source_offset`：来源定位和排障信息。
- `occurred_at_ms`、`processed_at` 和固定集合的 `disposition`。

不自动清理该表。只要还允许旧 Kafka 事件重放，删除去重收据就可能使统计重复；后续只有在形成明确的
retention 和重放水位策略后才增加清理流程。

### `short_link_access_totals`

主键为 `(short_link_id, result)`，保存累计次数、最早事件时间、最近事件时间。`short_link_id` 关联
`short_links.id`，不使用任意外部短码作为无界聚合维度。

### `short_link_access_hourly`

主键为 `(short_link_id, bucket_start_epoch, result)`，保存 UTC 小时分类计数。天趋势在查询时按 UTC 天对
小时数据汇总，避免同时维护两份时间投影。

数据库迁移必须同时覆盖：

- 空数据库通过 Docker init 创建完整 schema。
- 已存在 v1.8 `short_links` 数据的原地升级。
- 应用回滚时旧版本忽略新增表；删除统计表属于单独、显式且需备份的破坏性操作。

## 幂等事务与 offset

有效消息的处理顺序固定为：

```text
START TRANSACTION
  -> INSERT IGNORE processed_access_events(event_id, ...)
  -> 已存在：COMMIT，返回 duplicate
  -> not_found：记录 ignored disposition 后 COMMIT
  -> 查询 short_links.id；无法关联的非 not_found 事件回滚并进入 DLQ
  -> UPSERT short_link_access_totals
  -> UPSERT short_link_access_hourly
  -> 更新收据 disposition=aggregated
COMMIT
  -> 同步提交该 Kafka message 的 offset
```

关键崩溃窗口：

- MySQL 提交前崩溃：事务回滚，消息未提交 offset，重启后重新处理。
- MySQL 提交后、offset 提交前崩溃：消息会重放，`event_id` 判重后不再增加统计。
- offset 提交后崩溃：统计和收据已经提交，不丢失已处理副作用。

这是统计投影层面的 effectively-once，不改变 producer fail-open 和 Kafka retention 所决定的上游边界。

## 失败分类与 DLQ

MySQL 连接、查询、事务或 Kafka 临时错误一律视为可重试基础设施故障：

- 不提交源 offset。
- 使用可配置的指数退避，单次等待和尝试次数均有上限。
- 同一进程达到最大处理尝试后失败退出，由 Compose 或部署环境重启。
- 不通过匹配数据库错误字符串把未知 SQL 错误错误地分类成永久消息问题。

以下固定消息错误进入 `hao-shortlink.access-events.dlq.v1`：

- `invalid_json`
- `unsupported_schema`
- `unsupported_event_type`
- `invalid_contract`
- `key_code_mismatch`
- `orphan_short_link`

DLQ 使用版本化 envelope，保存来源 topic、partition、offset、固定原因码、失败时间和有界编码后的原始 key / payload。
consumer 必须在 DLQ delivery 成功后才提交源 offset。DLQ publish 与源 offset 不是跨 topic 原子事务，极端崩溃
窗口可能产生重复 DLQ record；下游应使用来源坐标和可用的 `event_id` 识别重复。

DLQ 不接收 MySQL 或 Kafka 基础设施故障，避免把尚未处理的业务事件错误地标记为永久失败。

## 查询 API

v1.9 提供内部接口：

```text
GET /internal/short-links/{code}/statistics
```

查询参数：

- `from`：可选，UTC 时间，包含；默认对齐后的 `to` 前 7 天。
- `to`：可选，UTC 时间，不包含；默认向上对齐到当前 UTC 小时或 UTC 天边界。
- `interval`：`hour` 或 `day`，默认 `day`。
- 小时趋势最大 31 天，天趋势最大 366 天。
- 显式 `from` / `to` 必须与所选 UTC 小时或 UTC 天边界对齐；v1.9 不把整桶数据伪装成任意秒级精确范围。

响应同时返回全量 summary 和所选范围的 trend。不存在短链返回 `404`；存在但无事件时返回零计数和空 trend。
接口只在 MySQL 统计配置启用时注册，并继续被 Nginx 的 `/internal/` 边界阻断。v2.0 加入用户、归属与权限后，
再决定是否增加公开接口。

## Consumer 进程与配置

v1.9 将 v1.8 consumer 的单文件实现拆为可单测组件：

- 配置加载和验证。
- Kafka 消费循环与手动 offset。
- MySQL 统计 writer。
- DLQ publisher。
- 重试策略。
- consumer 指标和健康状态。

统计 consumer 使用新 group `hao-shortlink-access-statistics-v1`，不能复用 v1.8 logger group，否则升级时会跳过
该 group 已经提交的 retained events。默认 `auto.offset.reset=earliest`，只回填 Kafka 当前仍保留的事件。

配置分为 VM、本地容器和测试容器三类，至少包含 Kafka、DLQ、MySQL、重试、poll、shutdown、健康和指标端口。
consumer 可以依赖 MySQL 与 Kafka，但 HTTP server 的跳转结果和 readiness 不能依赖统计 consumer。

## 可观测性

consumer 提供独立、只在 Compose 网络内抓取的健康和 `/metrics` 入口。至少包括：

```text
haohttp_shortlink_access_consumer_messages_total{result}
haohttp_shortlink_access_consumer_retries_total{operation}
haohttp_shortlink_access_consumer_dlq_total{result}
haohttp_shortlink_access_consumer_lag{partition}
haohttp_shortlink_access_consumer_last_success_unixtime
```

固定集合：

- message result：`aggregated`、`duplicate`、`ignored`、`dlq`、`error`
- retry operation：`mysql`、`offset_commit`、`dlq_publish`
- DLQ result：`success`、`failure`

topic、code、event ID、request ID、URL、错误文本和原始 payload 都不能成为 label。partition 是 topic 固定分区集合，
允许作为 lag gauge 的有界 label。持续故障日志继续限频。

## 重放与重建

v1.9 验证两类操作：

1. 幂等重放：重置测试 group offset 后重新消费，统计不变且 duplicate 指标增加。
2. 隔离重建：使用独立数据库和新 consumer group 从 earliest 重建 retained events，并与预期统计对比。

当前 access event topic retention 为 7 天。因此生产式历史重建只能覆盖仍在 Kafka 中的范围；v1.9 不因为演示
重放而悄悄宣称永久历史可恢复，也不提供默认清空当前统计表的脚本。任何会删除当前投影的数据操作都必须显式
指定测试数据库和允许开关，并在运行手册中标出不可逆边界。

## 验证要求

### 单元与数据库集成

- UTC 小时/天桶、乱序最早/最近时间、结果计数和 API 参数边界。
- 首次事件、重复 `event_id`、同一短码不同事件、`not_found` ignored 和 orphan DLQ 分类。
- MySQL 事务回滚、数据库提交后 offset 失败的重复恢复、consumer 重启续读。
- 空数据库初始化和 v1.8 schema 原地迁移。

### Kafka 与故障验证

- producer -> Kafka -> consumer -> MySQL -> 查询 API 完整闭环。
- MySQL 停止期间不提交 offset，恢复后统计补齐且不重复。
- 非法 JSON、不支持 schema、key 不匹配和 orphan 事件进入 DLQ。
- DLQ 不可用时源 offset 不前移。
- lag 在 consumer 停止或 MySQL 故障时增长，恢复消费后回落。
- 幂等重放不改变统计；隔离重建在 retention 范围内得到相同结果。

### 回归与发布

- Kafka / consumer / 统计关闭时，现有 v1.8 和更早回归继续通过。
- Kafka、MySQL 或统计 consumer 故障不改变 redirect HTTP 结果和 server readiness 语义。
- Linux VM、完整 Compose、干净源码目录、Ubuntu 22.04 Docker 构建和 GitHub Actions 均通过。
- 文档只在对应证据真实完成后将各批次标为已完成。

## v1.9 执行批次

1. `v1.9.0` 设计与迁移契约：本文件、路线图、决策和测试边界。
2. `v1.9.1` 数据模型与 MySQL 幂等投影。
3. `v1.9.2` consumer 统计写入、重试和 offset 顺序。
4. `v1.9.3` 内部统计查询 API。
5. `v1.9.4` DLQ、consumer 指标、lag 和健康语义。
6. `v1.9.5` 幂等重放、隔离重建、Compose 和 CI。
7. `v1.9.6` 全量、故障、干净目录、发布检查和文档收口。
