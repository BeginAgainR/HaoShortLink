# 决策记录

状态：持续维护
用途：记录项目中已经确认、暂定、待决策或暂缓的工程决策。

## 状态说明

- 已确认：当前阶段按此执行。
- 暂定：当前先采用，后续可根据实现和验证结果调整。
- 待决策：已发现问题，但尚未选择方案。
- 暂缓：当前阶段不处理。

## 已确认

### 文档语言

状态：已确认

README 和 `docs/` 使用中文。

### 文档目录

状态：已确认

公开文档放在 `docs/` 下，README 作为项目入口和文档导航。

### 项目分层

状态：已确认

- `HttpServer/` 保留为现有 HTTP 框架层。
- `apps/shortlink_server/` 作为短链接业务服务目录。
- 暂时不重命名 `HttpServer/`。

### ShortLink V1 范围

状态：已确认

V1 先实现最小闭环：

- 健康检查。
- 创建短链接。
- 根据短码跳转。

### ShortLink v1.1 范围

状态：已确认

v1.1 在 V1 最小闭环基础上补充持久化和查询缓存：

- MySQL 持久化短链映射。
- Redis 缓存短码跳转查询结果。
- 服务重启后，已持久化短链仍可跳转。

v1.1 不处理用户系统、访问统计、过期策略、限流、Docker Compose、Nginx、消息队列或监控。

### ShortLink v1.2 范围

状态：已确认

v1.2 在 v1.1 持久化和查询缓存基础上补充本地工程化运行环境：

- Docker Compose 编排 MySQL、Redis、`shortlink_server` 和 Nginx。
- Nginx 作为本地统一 HTTP 入口，反向代理到 `shortlink_server`。
- 保留 `shortlink_server` 直连调试入口。
- 补充 Compose、容器内运行和 VM 手工运行的配置样例。

v1.2 不处理完整线上部署、HTTPS/TLS 终止、发布回滚、日志采集、监控告警或压测。

### ShortLink v1.3 范围

状态：已确认

v1.3 在 v1.2 本地工程化运行环境基础上补充第一版可重复测试体系和 CI：

- CMake / CTest 测试入口。
- 第一批框架基础测试和短链业务纯逻辑测试。
- API 冒烟测试脚本。
- MySQL / Redis 集成测试脚本和 Redis 不可用回退测试脚本。
- Compose 依赖编排测试脚本。
- GitHub Actions 第一版 workflow，覆盖 Linux 构建、CTest 和 API 冒烟测试。

v1.3 不把 MySQL / Redis Compose 集成测试、Nginx 验证、压测、限流或监控接入第一版 CI；这些能力后续作为 v1.4 及之后的增强项处理。

### ShortLink v1.4 范围

状态：已确认

v1.4 在 v1.3 第一版测试与 CI 基础上补充性能与稳定性基线：

- 建立可重复压测入口和结果记录格式。
- 对健康检查、创建短链和短码跳转进行压测。
- 对比内存存储、MySQL 持久化、MySQL + Redis 查询缓存三种模式。
- 覆盖 Redis 不可用、MySQL 不可用、非法请求高并发和短码不存在高并发等异常场景。
- 根据压测结果评估 Redis 连接复用或连接池、MySQL 连接池参数、worker 线程数量和连接等待超时。
- 补充 Nginx 入口底线验证，确认反向代理链路没有被当前变更破坏。
- 限流正式实现后置到 v1.6；v1.4 只记录是否存在必须提前处理的流量保护风险。

v1.4 不处理 Prometheus、Grafana、完整指标系统、用户系统、访问统计、消息队列或后台管理；不在缺少压测证据时大规模重写 `HttpServer/` 或连接池；不声明生产最大承载能力。

v1.4 收口决策：

- 当前 Redis 固定延迟已定性为 OrbStack VM 到 Docker 发布端口的环境特定地址路径问题，不通过业务代码或连接池改写规避。
- v1.4 不实现 Redis 连接池；只有后续在非环境特定路径上出现明确的连接开销或资源瓶颈证据时再评估。
- MySQL 连接池参数矩阵、worker 数量关系和连接等待超时不在 v1.4 继续扩展，后续按新的资源等待证据立项。
- 限流保持在 v1.6，不能用限流掩盖业务正确性或依赖故障问题。

### v1.5 之后的版本顺序

状态：已确认方向，具体批次持续细化

- MySQL / Redis 集成 CI 已在进入 v1.5 前完成前置版；v1.5 聚焦可观测性，并持续维护依赖回归。
- v1.6 在可观测数据基础上实现可靠性与流量保护，补充限流、降级和健康检查语义。
- v1.7 先补链接状态、过期、详情 / 列表和缓存失效，再引入新的消息基础设施。
- Kafka 访问事件顺延到 v1.8，访问统计和相关工程化收口进入 v1.9。
- v2.0 聚焦用户、链接归属、对象级权限、自定义短码、最小管理页面、OpenAPI 和应用层收口。
- v2.1 在 v2.0 应用边界稳定后补 Kubernetes 运行闭环和多副本验收，不把生产级有状态集群作为第一版目标。
- v2.2 使用 Transactional Outbox 可靠发布本来就写 MySQL 的生命周期事件，以生命周期审计投影形成真实下游，
  并完成阶段终验与发布收口。
- v2.2 之后冻结本阶段功能范围；Schema Registry、RabbitMQ、Go、搜索 / 分析引擎和云基础设施只保留为条件 backlog。

v1.5 可观测性字段、指标、标签基数、`/metrics` 暴露和验证边界记录在 `docs/OBSERVABILITY_DESIGN.md`。

## 暂定

### API 与产品路径

状态：已确认

```text
/api/*             程序调用、认证和 owner 管理
/s/{code}          公共短码跳转
/app/              同源静态管理页面
/openapi.yaml      机器可读契约
```

说明：

- `/api/` 用于程序调用、认证和对象级管理接口。
- `/s/{code}` 用于用户访问和分享的短链跳转。
- v2.0 不再注册旧 `/internal/short-links...` 管理路由。

### 跳转状态码

状态：暂定

短码跳转默认使用：

```text
302 Found
```

说明：

302 表示临时重定向，适合作为短链接服务的默认行为，避免客户端过度缓存跳转结果。

### 错误响应格式

状态：暂定

框架层默认错误响应使用 JSON 格式：

```json
{
  "error": {
    "code": "not_found",
    "message": "Not Found"
  }
}
```

说明：

- `code` 用于程序判断错误类型。
- `message` 用于提供可读错误信息。
- 后续如需补充 `request_id`、`details` 等字段，可以在 `error` 对象内扩展。

### muduo 依赖范围

状态：已确认

当前项目只依赖 muduo 的核心库：

```text
muduo_base
muduo_net
```

说明：

- `HttpServer/` 是项目自己的 HTTP 框架层，不依赖 muduo 自带的 `muduo_http` 组件。
- 在当前 VM 的 GCC 15 环境下，muduo 全量构建可能因 `muduo_http` 的 warning-as-error 失败。
- 当前只安装和链接项目需要的核心库，后续如需其他 muduo 组件再单独决策。

### 配置文件格式

状态：暂定

当前配置加载使用简单 `key=value` 文本格式：

```text
server.name=HaoShortLink
server.port=8080
server.thread_num=4
log.level=INFO
```

说明：

- 空行和以 `#` 开头的整行注释会被忽略。
- key 和 value 两侧空白会被裁剪。
- 当前不支持 YAML、TOML、JSON、环境变量覆盖或热加载。
- 示例配置文件放在 `apps/shortlink_server/config/server.conf.example`。

### V1 短码生成策略

状态：暂定

V1 采用：

- 进程内自增序号。
- 将序号转换为 Base62。
- 短码左侧补齐到 6 位。

说明：

- 这是 V1 内存版的最小策略，适合先跑通创建和跳转闭环。
- 当前策略简单、可预测，不作为长期工业级方案。
- 后续接入持久化、多实例或更高安全要求后，可以升级为数据库发号、Redis INCR、Snowflake、随机 Base62 短码或其他生成策略。
- 短码生成逻辑应封装在业务服务内，避免 HTTP 路由直接依赖具体生成策略。

### v1.1 短码生成策略

状态：暂定

v1.1 计划采用：

```text
MySQL AUTO_INCREMENT id -> Base62(code)
```

说明：

- V1 的进程内自增序号在服务重启后会回到初始值，接入 MySQL 后可能重复生成已有短码。
- MySQL 自增 id 是持久化发号来源，服务重启后不会丢失已发出的编号。
- 当前策略简单、可验证，适合 v1.1 单实例持久化阶段。
- 该策略生成的短码可预测，后续如有安全或多实例发号要求，需要重新评估。

### v1.1 Redis 使用边界

状态：暂定

v1.1 中 Redis 只作为短码跳转查询缓存：

```text
shortlink:{code} -> original_url
```

说明：

- MySQL 是短链映射的事实来源。
- Redis 命中可以加速跳转查询。
- Redis 未命中必须继续查 MySQL，不能直接返回 404。
- Redis 不可用时，服务应尽量降级到 MySQL 查询。
- v1.1 暂不使用 Redis 做限流、发号、分布式锁或访问统计。

### v1.7 链接生命周期与内部接口边界

状态：已确认并实现

决策：

- 持久化状态只包含 `active` 和 `disabled`；过期由可空的 UTC `expires_at` 动态计算，不持久化 `expired` 状态。
- 已有记录迁移后默认 `active` 且永不过期，不改变既有短链跳转结果。
- 公开跳转对不存在、禁用和过期统一返回 `404`，日志和指标内部保留具体结果。
- 创建接口接受可选未来过期时间；内部接口提供详情、cursor 列表以及状态 / 过期时间更新。
- v1.7 不提前实现用户、链接归属或 RBAC。`/internal/` 由 Nginx 显式阻断，应用直连端口只绑定本机；正式身份与权限体系留到 v2.0。
- Redis 缓存完整生命周期记录，旧版纯 URL 缓存按 miss 淘汰；生命周期修改后同步失效缓存，异常并发下接受由 TTL 限制的有界最终一致。

理由：

- 两个持久化状态足以覆盖第一阶段启停能力，避免状态与时间共同表示过期而产生双重事实来源。
- 公开统一 404 可以避免暴露短码是否曾存在。
- 网络隔离能满足当前本地和可信内部管理需求，同时避免把 v2.0 的用户权限系统塞入 v1.7。
- 缓存旁路和同步失效延续现有架构，不为生命周期功能引入分布式事务或新的基础设施。

详细设计：`docs/LIFECYCLE_DESIGN.md`。

### v1.8 消息队列优先级

状态：已实现并完成本地故障验证与 GitHub Actions 云端 Kafka CI 验证

决策：

```text
GET /s/{code}
  -> shortlink_server
  -> Kafka access-events topic
  -> shortlink_event_consumer
```

- v1.8 使用 Kafka 承接短码跳转产生的访问事件；RabbitMQ 不作为第一批消息队列目标。
- 事件覆盖 `success`、`not_found`、`disabled`、`expired` 和 `error`，使用版本化 JSON 和独立 `event_id`。
- record key 使用短码，使同一短码事件进入同一 partition；第一版不包含原始 URL、IP、User-Agent 或 Referer。
- publisher 位于应用 handler 边界，不进入 `ShortLinkService`、repository 或 `HttpServer/`。
- 使用 librdkafka 非阻塞 producer、有界队列、独立 poll、delivery callback、idempotent producer 和有界退出 flush。
- Kafka 故障采用 fail-open，不改变跳转 HTTP 结果，也不影响 liveness / readiness。
- 使用独立消费者进程、consumer group 和手动 offset；连续三次提交失败后退出，避免后续提交跨过失败消息；
  v1.8 消费者只校验并记录事件，不写统计。
- 本地编排使用单节点 KRaft Kafka、显式 topic 初始化、固定镜像和只绑定 localhost 的 Kafka UI。
- v1.8 接受有明确观测的事件丢失和重复，不承诺端到端 exactly-once。

理由：

- 短链接跳转天然会产生访问事件，适合使用事件流承接访问统计、审计和后续异步处理。
- Kafka 更贴合访问事件流、日志流和大规模消费者扩展场景。
- 访问统计允许第一阶段采用 best-effort 事件，而跳转可用性和延迟不能依赖 Kafka。
- 独立 consumer 能保持 HTTP 进程职责清晰，并为 v1.9 统计副作用提供自然扩展点。
- 版本化 JSON 足以覆盖单生产者和单消费者，暂不需要 Schema Registry。
- 单节点编排足以验证 topic、partition、offset、consumer group、重放和故障降级，不声明生产高可用。

详细设计：`docs/ACCESS_EVENT_DESIGN.md`。

### v1.8 服务优雅退出与框架生命周期修复

状态：已修复并完成本地验证

决策：

- 服务进程阻塞 `SIGINT` / `SIGTERM`，由专用等待线程通知主 `EventLoop` 退出，再执行 producer 有界 flush。
- `HttpServer` 中 `EventLoop` 必须先于持有其指针的 `TcpServer` 构造，并晚于后者析构。
- 只调整成员声明顺序，不向 `HttpServer/` 引入 Kafka 类型、配置或业务逻辑。

理由：

- 初次加入优雅退出后，Kafka disabled 的容器收到 `SIGTERM` 仍以 `139` 退出，证明问题不依赖 Kafka。
- 原成员顺序会让 `TcpServer` 先构造、`EventLoop` 后构造，并在析构时先销毁 `EventLoop`，其持有关系存在未定义行为。
- 调整后同一信号探针以 `0` 退出并记录 `event=server_shutdown`，Kafka producer 的有界 flush 才有可靠生命周期边界。

### v1.9 消费幂等、失败处理与重放

状态：已确认并实现；本地故障回归和 GitHub Actions 云端 CI 均已通过

决策：

- v1.9 才将访问事件写入 MySQL 统计模型，v1.8 不提前制造无业务副作用的幂等表和 DLQ。
- 使用独立 `event_id`、MySQL 唯一约束和事务，使重复事件不会重复增加统计。
- 统计事务成功后再提交 Kafka offset；MySQL 临时错误不提交 offset，并执行有界退避重试。
- 永久非法或不兼容事件进入 DLQ，避免 poison message 永久阻塞 partition。
- 增加 consumer lag、处理成功 / 失败、重复、重试和 DLQ 观测。
- 使用新 consumer group 或受控 offset 重置验证事件重放和统计重建。
- 业务访问次数只统计 `success`；尝试次数和结果分类只按能够关联到真实 `short_links.id` 的事件聚合。
- `not_found` 不按任意外部短码建立统计行，避免恶意扫描造成 MySQL 高基数膨胀；只保留幂等收据和低基数 ignored 观测。
- 查询 API 保持 `/internal/` 边界，在 v2.0 用户、归属和权限落地前不经 Nginx 公开。
- 当前 7 天 Kafka retention 只支持 retained range 内的重放；v1.9 使用隔离数据库验证统计重建，不提供默认清空在线投影的入口。

理由：

- 只有统计写入产生真实 MySQL 副作用后，消费幂等、重试和 DLQ 才有可验证的业务语义。
- `Kafka -> MySQL` 链路不能仅靠 Kafka producer 事务获得端到端 exactly-once；业务唯一约束更直接。
- 重放和统计重建能证明 Kafka 相比一次性任务队列的业务价值。

### v2.0 产品闭环与应用层边界

状态：已确认并实现；v2.0 本地验收已通过

决策：

- v2.0 必须交付基础身份与会话、链接归属、对象级权限、显式管理权限和受权限保护的管理 API。
- 最小管理页面是验收项，不再作为可选项；至少支持登录、创建、列表、禁用 / 恢复、过期设置和统计查看。
- 提供机器可读 OpenAPI 契约，并支持自定义短码；自定义短码必须处理保留路径、格式、唯一约束、并发冲突和归属权限。
- 使用轻量 schema migration 管理用户、会话和 owner 等变更，验证空库初始化与 v1.9 数据升级。
- 身份实现必须使用适合密码存储的哈希算法；随机会话 token 只保存不可逆摘要，具备过期与撤销语义，不记录凭据。
- 在应用层拆分过大的入口、handler、service 和组合职责；保持 C++17，不重写或业务化 `HttpServer/`。
- 公共短码跳转继续匿名可用，管理操作以对象授权为准，不能将隐藏 `/internal/` 路径当作正式权限系统。

理由：

- 当前工程能力已经丰富，但缺少普通用户可以操作和演示的产品闭环；最小页面能证明 API、权限、统计和生命周期协同工作。
- OpenAPI、迁移和安全边界比继续堆叠新中间件更直接地提升可维护性和项目完整度。
- v2.0 会显著增加 handler 与业务规则，先收敛应用层职责可以控制后续 Kubernetes 和 Outbox 的改动面。

非目标：Go 服务、OIDC、邮件、计费、自定义域名、复杂前端框架、多租户配额和独立分析平台。

补充决策：

- v2.0 不实现平台管理员或跨用户管理；“管理页面”是普通用户管理自己链接的产品入口。
- 用户使用用户名和密码，开放注册可配置；不引入邮箱与找回密码。
- `POST /api/short-links` 调整为登录后创建，公共 `GET /s/{code}` 保持匿名。
- v1.9 历史链接迁移给不可登录的保留系统账号，不允许普通用户自动认领。
- 浏览器使用同源 `HttpOnly` Cookie，MySQL 只保存随机会话 token 摘要；不使用 JWT。
- 管理页面采用 `/app/` 下的静态 HTML / CSS / JavaScript，不引入独立前端构建链。
- 详细 API、安全、迁移和应用边界见 `docs/PRODUCT_DESIGN.md`。

### v2.1 Kubernetes 部署边界

状态：已确认方向，尚未实现

决策：

- 固定一种本地可重复 Kubernetes 环境；第一版部署 `shortlink_server`、`shortlink_event_consumer` 和管理入口，
  只在 MySQL 持久化模式下验证应用多副本。
- 使用 ConfigMap、Secret、Service、健康探针和资源 requests / limits；入口在 Ingress 与复用 Nginx 之间按实现环境确认。
- MySQL、Redis 和 Kafka 可以使用外部依赖或仅供本地验收的单节点部署，不声明生产级 StatefulSet、存储或多节点高可用。
- 必须验证滚动升级、回滚、Pod 重建、多副本请求语义、consumer group 扩展和关键依赖故障。
- 必须提供可重复的启动、健康检查、演示和清理入口，并在干净环境中验证。
- HPA、Helm 打包和生产监控高可用只有在基础清单和负载证据稳定后再评估。

理由：

- 当前应用已有容器镜像、外部事实存储、健康检查和指标，具备验证 Kubernetes 应用部署的基础。
- 先验证应用工作负载和发布语义，可以避免把数据库、缓存和消息集群运维混入第一版学习目标。
- 多副本验收能够验证 Redis 全局限流、MySQL 事实存储和 Kafka consumer group 的现有设计边界。

### v2.2 生命周期事件 Transactional Outbox

状态：已确认方向，尚未实现

决策：

- 使用 Transactional Outbox 可靠发布链接创建、禁用、恢复、过期时间或归属变更等生命周期事件。
- Outbox 只用于本来就写 MySQL 的生命周期操作；访问事件来自读路径，不为每次跳转增加同步 Outbox 写入。
- 业务表和 outbox 在同一 MySQL 事务中提交；relay 负责发布、退避重试、积压观测和清理。
- 接受 relay 发布成功但状态回写失败造成的重复发布，继续使用 `event_id` 和消费者幂等处理。
- 生命周期事件由独立职责的 consumer 幂等写入可查询审计投影，并通过管理 API / 页面展示；不创建没有真实下游的展示性 topic。
- v2.2 同时执行功能、安全、迁移、浏览器端到端、Kubernetes、故障注入、性能、长稳和干净环境终验。
- 不声明跨 MySQL 与 Kafka 的端到端 exactly-once。

理由：

- 生命周期更新可以在同一 MySQL 事务中原子写业务表和 outbox，适合可靠发布。
- 在访问读路径增加 outbox 会重新引入每次跳转的同步数据库写入，削弱 Kafka 解耦价值。
- v1.9 已建立 `event_id` 幂等、重试、DLQ 和重放边界，可以承接 outbox 的至少一次发布语义。
- 审计投影让生命周期事件具备用户可见的业务结果，也能验证 relay、Kafka 与幂等消费者的完整恢复链路。

### 阶段终点与条件 backlog

状态：已确认边界

决策：

- v2.2 达到 `docs/FINAL_ACCEPTANCE.md` 的完成定义后发布阶段终点，并冻结功能范围，后续优先维护、演示和缺陷修复。
- 只有出现多个跨语言消费者或频繁 schema 演进后，才评估 Schema Registry、Avro、Protobuf 或 JSON Schema。
- 只有 Kafka 无法覆盖新的投递语义且有明确消费者后，才评估 RabbitMQ。
- Go、Elasticsearch / ClickHouse、Terraform、公有云、Service Mesh、自定义域名、OIDC 和计费没有预分配版本号。
- 条件 backlog 不属于 v2.0-v2.2 验收，启动前必须重新确认真实需求、替代方案和可验证收益。

理由：

- 单生产者、少量消费者阶段使用 `schema_version` 已足够；重复引入同类消息队列或无数据规模支撑的分析组件只会扩大维护面。
- 明确终点能避免项目无限堆栈，让已引入的组件都有可运行、可观测、可恢复和可解释的证据。

### v1.6 限流与健康语义

状态：已确认

决策：

- 第一版限流只保护 `POST /api/short-links`，不限制短码跳转、健康检查或指标入口。
- 使用 Redis Lua 固定窗口和跨实例共享的全局创建额度。
- 超限返回 `429 Too Many Requests`、统一 JSON 错误和 `Retry-After`。
- Redis 限流故障采用 fail-open；Redis 不是业务事实存储，故障必须可观察但不应将所有创建请求拒绝为不可用。
- 不在缺少可信代理和客户端身份边界时直接信任 `X-Forwarded-For` 或 `X-Real-IP`，因此 v1.6 不实现按 IP 限流。
- `/api/health` 保留兼容并表示 liveness；新增独立 liveness / readiness 入口。
- MySQL 是 MySQL 存储模式的 readiness 必要依赖；Redis 查询缓存和限流均为可降级依赖。

理由：

- 全局额度能在不扩张客户端身份范围的前提下保护当前写入路径。
- 固定窗口有边界突刺权衡，但能以小而可验证的 Lua 原子逻辑满足当前阶段。
- 区分 liveness 和 readiness 可避免依赖故障导致无效的进程重启，同时让流量入口停止向不可用的 MySQL 实例送流量。

详细设计：`docs/RELIABILITY_DESIGN.md`。

## API 版本策略

状态：已确认

v2.0 继续使用 `/api`，不并行维护 `/api/v2`。只有未来出现必须同时兼容的破坏性协议版本时再重新评估。

## 暂缓

以下能力不属于当前阶段：

- 平台管理员、跨用户管理和 RBAC。
- 邮箱、验证码、密码找回、OIDC、邀请和计费。
- 完整线上部署。
- HTTPS/TLS 终止。
- 发布回滚。
- 生产告警、值班和长期容量规划。
