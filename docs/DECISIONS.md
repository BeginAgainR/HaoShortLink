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
- v2.0 聚焦用户、链接归属、权限、管理 API 和整体验收，不再一次承担链接生命周期与全部统计实现。

v1.5 可观测性字段、指标、标签基数、`/metrics` 暴露和验证边界记录在 `docs/OBSERVABILITY_DESIGN.md`。

## 暂定

### API 路径

状态：暂定

```text
GET  /api/health
POST /api/short-links
GET  /s/{code}
```

说明：

- `/api/` 用于程序调用的接口。
- `/s/{code}` 用于用户访问和分享的短链跳转。

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

### v1.8 消息队列优先级

状态：暂定

v1.8 消息队列能力优先规划 Kafka：

```text
shortlink_server -> access-events topic -> stats consumer
```

说明：

- 短链接跳转天然会产生访问事件，适合使用事件流承接访问统计、审计和后续异步处理。
- Kafka 更贴合访问事件流、日志流和大规模消费者扩展场景。
- RabbitMQ 仍作为后续任务队列、复杂路由或可靠任务分发场景的候选，但不作为第一批消息队列目标。
- 消息队列接入不能改变短码跳转主路径语义；Kafka 不可用时需要明确降级策略。

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

## 待决策

### API 版本

状态：待决策

当前暂不引入 `/api/v1`。后续如果需要兼容多个 API 版本，再重新评估。

## 暂缓

以下能力不属于当前阶段：

- 用户系统。
- 访问统计。
- 完整线上部署。
- HTTPS/TLS 终止。
- 发布回滚。
- 消息队列第一批目标已规划为 v1.8 Kafka 访问事件流；v1.7 先补链接生命周期。
- 监控和告警第一批目标已规划为 v1.5 可观测性，当前 v1.4 不实现。
