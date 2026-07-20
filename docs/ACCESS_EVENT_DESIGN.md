# 访问事件与 Kafka 设计

状态：已完成；v1.8.0-v1.8.6 本地实现、故障回归与 GitHub Actions 云端 CI 均已通过

## 业务目标

v1.8 在现有短码跳转链路上增加异步访问事件，为 v1.9 访问统计提供可重放的事件来源：

```text
GET /s/{code}
  -> ShortLinkService::resolve
  -> 立即形成 302 / 404 / 500 响应
  -> 异步写入 Kafka access-events topic
  -> shortlink_event_consumer 消费、校验并记录
```

Kafka 不是创建、跳转或生命周期能力的事实来源。MySQL 继续保存短链事实，Redis 继续作为可选查询缓存和
创建限流依赖；Kafka 只承接跳转产生的访问事件。Kafka 不可用时，短链跳转必须保持原有 HTTP 语义。

## v1.8 交付边界

v1.8 实现：

- 版本化访问事件模型和稳定 JSON codec。
- 应用层 publisher 抽象、Noop 实现和 librdkafka 异步 producer。
- 成功、不存在、禁用、过期和内部错误五类跳转结果的访问事件。
- 有界 producer 队列、delivery callback、有限时长退出 flush 和 fail-open。
- 单节点 KRaft Kafka、显式 topic 初始化、本地 Kafka UI 和 Compose 配置。
- 独立 `shortlink_event_consumer` 进程、consumer group、手动 offset 和基础失败处理。
- producer / consumer 结构化日志、低基数指标、故障测试、性能对照和 CI 验证。

v1.8 不实现：

- 统计表、统计聚合和统计查询 API。
- 消费结果写入 MySQL、业务幂等表、重试 topic 或 DLQ。
- Transactional Outbox、磁盘 WAL 或访问事件不丢失承诺。
- Kafka 事务或端到端 exactly-once。
- Schema Registry、Avro 或 Protobuf。
- 多 broker、高可用、TLS、SASL、ACL 或正式生产部署。
- IP、地理位置、User-Agent 或 Referer 分析。
- 不把 Kafka 类型或访问事件逻辑放入 `HttpServer/`。实现期间只调整了 `EventLoop` 与 `TcpServer` 的成员声明顺序，
  修复优雅退出暴露出的既有生命周期错误；该修复不属于 Kafka 业务耦合。

## 事件定义

topic 使用版本化名称：

```text
hao-shortlink.access-events.v1
```

Kafka record key 使用短码 `code`，让同一短码的事件进入同一 partition。第一版 payload：

```json
{
  "schema_version": 1,
  "event_type": "short_link_access",
  "event_id": "独立生成的事件标识",
  "occurred_at_ms": 1784304000123,
  "request_id": "HTTP request ID",
  "code": "000001",
  "result": "success",
  "http_status": 302
}
```

`result` 只允许：

```text
success
not_found
disabled
expired
error
```

约束：

- `event_id` 独立生成，不能使用允许客户端传入的 `request_id` 代替。
- `occurred_at_ms` 表示服务形成跳转结果的 UTC Unix 毫秒时间。
- “访问”表示服务处理了一次短码跳转请求，不表示客户端最终成功访问目标站点。
- 第一版不写入原始 URL、IP 或 User-Agent，避免无必要的敏感数据和不稳定字段进入事件契约。
- schema 只允许向后兼容地增加可选字段；破坏性变化使用新 schema 或新 topic 版本。

## Producer 设计

publisher 位于应用 handler 边界，不进入 `ShortLinkService` 或 repository：

```text
redirect handler
  -> ShortLinkService::resolve(code)
  -> RedirectHandler 映射最终结果并构造 AccessEvent
  -> AccessEventPublisher::publish(event) noexcept
  -> HttpResponse
```

这样可以同时获得 HTTP request ID、短码和最终状态，又不让 Kafka 污染核心短链业务。`resolve()` 抛出异常时，
handler 记录 `error` 事件并继续抛出，由现有 HTTP 错误边界形成 `500`。

producer 使用 librdkafka 非阻塞 produce：

- 请求线程只完成事件构造、JSON 序列化和本地入队，不等待 broker RTT。
- 禁止使用会在队列满时阻塞请求线程的 producer 选项。
- 独立 poll 线程驱动 delivery callback，区分已接受入队与最终投递结果。
- producer 队列、消息超时和退出 flush 都必须有上限。
- 启用 idempotent producer 和 `acks=all`，减少 producer 重试造成的 broker 内重复。
- Kafka 配置非法或 producer 初始化失败时降级到 Noop publisher，并记录可观察错误。
- Kafka 不参与 liveness 或 readiness；MySQL 仍是 MySQL 模式的唯一硬 readiness 依赖。

这条链路不承诺事件不丢失：进程可能在本地队列尚未投递时崩溃，broker 长时间不可用也可能导致消息超时或
队列满后丢弃。idempotent producer 也不等于 HTTP、Kafka、consumer 和 MySQL 的端到端 exactly-once。

## Consumer 设计

v1.8 新增独立 `shortlink_event_consumer` 可执行文件，避免把消费循环塞进 HTTP 服务进程。第一版职责：

- 使用固定 consumer group 订阅 access-events topic。
- 关闭自动 offset 提交。
- 校验 JSON、schema 版本、必填字段和枚举值。
- 有效事件完成结构化日志记录后提交 offset。
- 非法或不支持的事件记录可观察的 discard 后提交 offset，避免 poison message 永久阻塞 partition。
- Kafka 临时消费错误退避重试；offset 连续三次提交失败时进程失败退出，避免后续成功提交跨过失败消息；
  进程退出时执行有界关闭。
- 接受可能重复消费的事实，不用进程内集合伪装持久化幂等。

消费者在 v1.8 没有 MySQL 副作用，因此不提前实现业务重试 topic、DLQ 或去重表。v1.9 写入统计后，再以
`event_id`、MySQL 事务和正确的 offset 提交顺序实现真实幂等和失败处理。

## 代码组织约束

当前根 `CMakeLists.txt` 会递归收集 `apps/shortlink_server/src/*.cpp` 并排除已知的 HTTP server main。
消费者的第二个 `main()` 不能直接放入该目录，否则会进入错误的 executable。当前采用：

```text
apps/shortlink_server/include/shortlink/    共享事件契约和 producer 接口
apps/shortlink_server/src/                  HTTP 服务使用的事件 codec / producer 实现
apps/shortlink_event_consumer/src/          独立 consumer main 和消费循环
```

CMake 为 consumer 建立独立 target，并显式复用事件 codec 源文件。跳转事件映射收口在应用层
`RedirectHandler`，使五种结果和异常边界可以独立测试。v1.8 不借此把现有短链业务整体重构为新的库层级，
也不把 Kafka 类型暴露到 `HttpServer/`。Kafka 配置、publisher 生命周期和 consumer 配置放在独立文件中，
避免继续扩大当前 `main.cpp` 的 handler 细节。

## 本地编排

Kafka 使用独立 Compose overlay，避免现有默认 MySQL / Redis / 监控环境每次都启动消息组件：

```text
compose.yaml
+ compose.kafka.yaml
  -> single-node KRaft Kafka
  -> topic initializer
  -> shortlink_server with Kafka config
  -> shortlink_event_consumer
  -> Kafka UI
```

本地约束：

- Kafka 和 Kafka UI 镜像必须固定具体版本，不能使用 `latest`。
- 单节点 topic 使用 replication factor 1，只代表本地开发，不声明高可用。
- 显式创建 topic，不依赖 broker 自动创建。
- topic 第一版使用 3 个 partition、`cleanup.policy=delete` 和有限 retention。
- Kafka UI 只绑定宿主机 localhost，不经过 Nginx 暴露。
- `shortlink_server` 不依赖 Kafka 健康后才启动；consumer 可以等待 topic 初始化完成。
- 容器内使用 Compose 服务名，Linux VM 手工验证使用单独的宿主机发布 listener。

已验证的依赖组合：

- Linux VM：Ubuntu 26.04、librdkafka 2.13、nlohmann-json 3.11.3、kcat 1.7.1。
- GitHub Actions 目标镜像：Ubuntu 22.04、发行版 librdkafka 1.8.0、nlohmann-json 3.10.5；Docker 镜像构建已通过。
- Kafka：`apache/kafka:4.3.1`；Kafka UI：`ghcr.io/kafbat/kafka-ui:v1.5.0`。

## 可观测性

producer 已增加：

```text
haohttp_shortlink_access_event_enqueue_total{result}
haohttp_shortlink_access_event_delivery_total{result}
haohttp_shortlink_access_event_queue_size
```

固定结果集合：

```text
enqueue: accepted, queue_full, error
delivery: success, failure
```

consumer 第一版至少记录 processed、discarded 和 error 结构化日志。指标和日志不使用 topic、code、event ID、
request ID、URL、IP、User-Agent 或错误消息作为 label；持续故障日志必须限频。

## 验证要求

### 单元测试

- 事件构造、JSON 编解码、转义、必填字段和枚举校验。
- 独立 event ID 与 request ID 边界。
- Noop / fake publisher 不改变跳转结果。
- producer 指标并发更新和合法 label 组合。
- handler 在 repository 查询异常时发布 `error` 事件并继续抛给既有 HTTP 500 边界。

### Kafka 集成测试

- success、not_found、disabled、expired 和 error 事件内容正确。
- record key、schema 版本和 topic 正确。
- Kafka 启动前不可用、运行中停止和恢复时，HTTP 结果与 readiness 不变。
- 队列满时请求不阻塞，丢弃可观察。
- delivery success / failure callback 和有界 shutdown 行为正确。
- consumer 正常消费、非法事件 discard、手动 offset、重启续读和重复消息边界正确。
- Kafka UI 能连接本地 broker 并查看 topic；不通过 Nginx 对外暴露。

### 回归与性能

- Kafka 关闭时，v1.7 全量测试继续通过。
- 比较 Kafka 关闭、正常和故障三组 redirect 基线，记录 QPS、平均延迟、P95 / P99 和错误率。
- Kafka 故障不能引入跳转 5xx 或依赖 broker RTT 的固定等待。
- Linux VM、容器、干净克隆和 GitHub Actions 使用固定依赖版本完成验证。

## v1.8 执行批次

1. `v1.8.0` 设计与依赖验证：
   - 状态：已完成。
   - 已固化本文件、决策、路线图和测试边界。
   - 在 Linux VM 验证发行版 librdkafka、目标 Kafka 镜像和 CMake / Docker 依赖链。
2. `v1.8.1` 事件契约与抽象：
   - 状态：已完成。
   - 实现 AccessEvent、codec、event ID、publisher 接口、Noop / fake 和单元测试。
3. `v1.8.2` 异步 producer：
   - 状态：已完成。
   - 实现 librdkafka producer、配置、poll、delivery callback、指标、有界关闭和 handler 接入。
   - 验证 Kafka disabled、初始化失败和队列满不会改变 HTTP 结果。
4. `v1.8.3` Kafka 本地编排：
   - 状态：已完成。
   - 增加 Compose overlay、KRaft Kafka、topic 初始化、Kafka UI、固定镜像和运行手册。
5. `v1.8.4` 独立 consumer：
   - 状态：已完成。
   - 实现消费循环、schema 校验、手动 offset、错误退避、discard 和结构化日志。
6. `v1.8.5` 故障与可观测性收口：
   - 状态：已完成本地验证，Kafka CI job 已配置并通过。
   - 覆盖 broker 中断、队列满、重复、非法事件、consumer 重启和 shutdown。
   - 接入 Kafka 集成 CI，并完成三模式性能对照。
7. `v1.8.6` 全量回归与文档收口：
   - 状态：已完成；GitHub Actions run `29637356364` 三个 job 均已通过。
   - Linux VM、完整 Compose、Kafka 故障恢复、三模式性能和干净目录回归均已通过。
   - 干净源码目录：`/Users/hao/Code/haoHTTP-v18-clean`；VM 构建目录：`/tmp/haoHTTP-v18-clean-build`。

## 后续升级

v1.9 将真实消费副作用落到访问统计：

- 统计表、按短码和时间窗口聚合、最近访问时间和查询 API。
- 使用 `event_id` 唯一约束、MySQL 事务和处理后提交 offset 实现消费幂等。
- MySQL 临时错误退避重试；永久非法消息进入 DLQ。
- 增加 consumer lag、处理成功 / 失败、重复和 DLQ 指标。
- 通过新 consumer group 或受控 offset 重置验证事件重放与统计重建。

v2.2 已规划在链接创建、禁用、恢复、过期时间或归属变更等本来就写 MySQL 的生命周期操作上使用
Transactional Outbox，并由独立 relay 和生命周期审计 consumer 形成可查询下游。访问事件来自读路径，
不计划为每次跳转增加 Outbox 同步写入。Schema Registry 不再预分配版本号，只有出现多个跨语言消费者
或频繁 schema 演进后才作为阶段终点之后的条件 backlog 评估。
