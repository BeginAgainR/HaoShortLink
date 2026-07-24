# 路线图

状态：持续维护；v2.0 已完成本地验收，v2.1-v2.2 保持计划状态
说明：本路线图会随着实现、验证结果和需求变化持续调整。

## 版本规划草案

版本号用于标记项目达到的可交付状态，不代表后续计划已经锁死。短链接业务是当前主线场景，
后续版本会围绕该场景逐步引入常见后端工程组件。远期版本保持草案状态，随着实现和验证结果持续调整。

| 版本 | 状态 | 目标能力 | 计划引入技术 |
| --- | --- | --- | --- |
| v0.1 | 已完成 | 项目清理、公开文档基线、旧业务残留记录 | Markdown 文档、git 基线提交 |
| v0.2 | 已完成 | 框架基础能力补强 | 请求日志、统一错误响应、JSON 响应辅助、配置加载 |
| v1.0 | 已完成 | ShortLink 最小闭环 | HTTP API、短码生成、内存存储、302 重定向 |
| v1.1 | 已完成 | 持久化与缓存 | MySQL、Redis、SQL 脚本 |
| v1.2 | 已完成 | 工程化运行 | Docker Compose、Nginx、配置样例、部署文档 |
| v1.3 | 已完成 | 测试与 CI | 框架测试、API 测试、集成测试、CI |
| v1.4 | 已完成 | 性能与稳定性 | 压测、连接资源评估、错误场景覆盖、Nginx 入口底线验证 |
| v1.5 | 已完成 | 可观测性与依赖 CI | 结构化日志、指标统计、Prometheus、Grafana、MySQL / Redis 集成 CI |
| v1.6 | 已完成 | 可靠性与流量保护 | Redis 限流、降级策略、健康语义、保护性测试 |
| v1.7 | 已完成 | 链接生命周期 | 链接状态、过期策略、详情与列表、缓存失效 |
| v1.8 | 已完成 | 消息队列与异步事件 | Kafka、Kafka UI、异步生产、独立消费、故障降级与观测 |
| v1.9 | 已完成 | 访问统计与工程化增强 | 幂等统计、查询 API、重试与 DLQ、Lag、重放与发布检查 |
| v2.0 | 已完成（本地验收） | 产品闭环与应用层收口 | 用户与权限、链接归属、自定义短码、管理 API、最小管理页面、OpenAPI |
| v2.1 | 草案 | Kubernetes 运行闭环 | 应用工作负载、多副本、配置与凭据、探针、入口、发布与故障恢复 |
| v2.2 | 草案 | 可靠事件闭环与阶段终验 | Transactional Outbox、relay、生命周期审计投影、恢复验证、最终压测与发布检查 |

当前策略：

- 近期版本只约束方向和边界，不提前锁死实现细节。
- v1.0 之前优先保持框架稳定和项目可 review。
- v1.2 之后的内容必须等 v1.1 持久化与缓存验证后再细化。
- v1.4 先建立可重复的性能与稳定性基线，再根据数据决定是否优化连接池；限流正式实现后置到 v1.6。
- MySQL / Redis 已是核心依赖，集成 CI 前移到 v1.5，不等到所有基础设施接入后再集中补齐。
- 链接状态、过期和查询等核心业务能力前移到 v1.7；Kafka 顺延到 v1.8，避免业务闭环仍较薄时先承担消息系统复杂度。
- v1.8 只建立访问事件管道，不提前写统计；Kafka 采用异步 fail-open，不能改变跳转和 readiness 语义。
- v1.9 在产生真实 MySQL 消费副作用后实现 `event_id` 幂等、失败重试、DLQ、consumer lag 和受控重放。
- 后续引入新后端组件时，应优先采用能长期维护和扩展的真实组件方案；只有在复杂度明显超出当前版本目标时，才使用文档记录降级或后置决策。
- v2.0 作为第一个功能相对齐全的短链接服务里程碑，应补齐用户、归属、对象级权限、自定义短码、
  可操作的最小管理页面和机器可读 API 契约；同时收敛应用层职责，但不重写 `HttpServer/`。
- v2.1 在 v2.0 应用边界稳定后补 Kubernetes 部署和多副本验收；第一版聚焦应用工作负载，不声明生产级 MySQL、Redis 或 Kafka 集群能力。
- v2.2 使用 Transactional Outbox 可靠发布本来就写 MySQL 的生命周期事件，并以可查询的生命周期审计投影
  证明事件有真实下游价值；完成全链路、迁移、Kubernetes、性能、稳定性和干净环境终验后冻结阶段性功能范围。
- Go、RabbitMQ、Schema Registry、Elasticsearch / ClickHouse、Terraform、公有云和生产级有状态集群不属于
  v2.0-v2.2 验收范围；后续只有出现真实需求和可验证收益时再进入条件 backlog。
- 未实现能力不得在 README 或 docs 中描述为已完成。

## 阶段 0：项目清理与文档基线

目标：

- 清理旧业务痕迹。
- 建立公开文档体系。
- 明确 `HttpServer/` 和 `apps/shortlink_server/` 的边界。
- 记录已知问题和待决策项。

## 阶段 1：框架基础能力补强

状态：已完成

已完成任务：

- 修复已知框架问题。
- 增加请求日志。
- 增加统一错误响应。
- 增加 JSON 响应辅助工具。
- 增加配置加载能力。

## 阶段 2：ShortLink V1 最小闭环

状态：已完成

目标：

- 启动真实 `shortlink_server` 服务，而不是 placeholder。
- 提供健康检查接口。
- 使用内存存储维护短码到原始 URL 的映射。
- 提供创建短链接接口。
- 根据短码跳转到原始 URL。

V1 只追求最小可用闭环，不包含用户系统、统计、持久化缓存、部署和压测。

### v1.0 执行批次

v1.0 按小步可 review 的方式执行，每一批尽量对应一个独立提交。

1. 服务入口和健康检查：
   - 状态：已完成。
   - `apps/shortlink_server/` 接入现有 `HttpServer`。
   - 加载基础配置或使用默认端口启动服务。
   - 注册 `GET /api/health`。
   - 在 Linux VM 中构建并用 curl 验证健康检查。
2. 内存版短链核心：
   - 状态：已完成。
   - 新增短链接业务服务。
   - 使用内存结构保存 `code -> original_url`。
   - 实现短码生成和最小 URL 校验。
   - 明确进程重启后数据丢失。
3. 创建和跳转接口：
   - 状态：已完成。
   - 实现 `POST /api/short-links`。
   - 实现 `GET /s/{code}`。
   - 命中时返回 `302 Found` 和 `Location`。
   - 参数错误或短码不存在时返回 JSON 错误。
   - 在 Linux VM 中构建并用 curl 跑通创建和跳转闭环。
4. 文档与收口验证：
   - 状态：已完成。
   - 更新 README 和 `docs/` 中的实现状态。
   - 记录 V1 内存版限制和验证结果。
   - 确认未将 MySQL、Redis、Docker、Nginx 等后续能力描述为已完成。
   - 在 Linux VM 中进行最终构建和手工接口验证。

### v1.0 非目标

- MySQL 持久化。
- Redis 缓存或限流。
- Docker Compose。
- Nginx。
- 用户系统、登录或权限。
- 链接列表、删除或更新。
- 访问统计。
- 过期策略。
- 压测。
- 复杂短码策略。
- 完整自动化测试体系。

## 阶段 3：持久化与缓存

计划能力：

- MySQL 持久化短链接数据。
- Redis 缓存短码查询结果。
- 根据需要补充基础数据脚本。

当前状态：已完成。

### v1.1 执行批次

v1.1 目标是让 V1 短链映射从进程内存升级为可持久化、可缓存。MySQL 是短链数据的
事实来源，Redis 只用于加快短码跳转查询，不改变接口语义。

1. 设计收口：
   - 状态：已完成。
   - 明确 `short_links` 表结构草案。
   - 明确短码生成策略从进程内自增迁移为 MySQL 自增 id 生成 Base62 短码。
   - 明确 Redis 只缓存 `code -> original_url` 查询结果，不承担限流、发号或事实存储职责。
   - 明确 v1.1 非目标。
2. 存储层抽象：
   - 状态：已完成。
   - 抽象短链接 repository 接口。
   - 将当前内存版实现迁移为 repository 实现，保持 V1 行为不变。
   - 通过 Linux VM 构建和 V1 curl 验证确认没有行为回退。
3. MySQL 表结构和创建持久化：
   - 状态：已完成。
   - 新增 `short_links` 建表脚本。
   - 创建短链接时写入 MySQL。
   - 使用 MySQL 自增 id 生成短码并保存。
4. MySQL 跳转查询：
   - 状态：已完成。
   - 短码跳转从 MySQL 查询原始 URL。
   - 验证服务重启后已创建短链仍可跳转。
5. Redis 查询缓存：
   - 状态：已完成。
   - 跳转时先查 Redis，未命中再查 MySQL。
   - MySQL 命中后回填 Redis。
   - Redis 不可用或未命中时不应直接改变为 404。
6. 文档与验证收口：
   - 状态：已完成。
   - 更新运行手册、数据模型和阶段状态。
   - 补充 MySQL/Redis 相关验证记录。
   - 做远端干净克隆验证。

### v1.1 非目标

- 用户系统、登录或权限。
- 链接列表、删除或更新。
- 访问统计。
- 过期策略。
- Redis 限流。
- Docker Compose。
- Nginx。
- 压测。
- 消息队列。
- 监控和告警。

## 阶段 4：工程化运行环境

计划能力：

- Docker Compose。
- Nginx。
- 配置样例。
- 部署文档。

当前状态：已完成。

### v1.2 执行批次草案

v1.2 目标是把当前手工启动和验证流程整理为更标准的本地工程化运行方式。

1. Docker Compose：
   - 状态：已完成。
   - 已完成 MySQL、Redis 的本地 Docker Compose 依赖编排。
   - 已验证 `haoHTTP` 通过 `docker.orb.internal` 连接 OrbStack Docker 中的 MySQL 和 Redis。
   - 已新增并验证 `shortlink_server` 服务容器配置。
   - 已新增 Nginx 反向代理配置，形成本地统一 HTTP 入口。
2. 配置整理：
   - 状态：已完成。
   - 已补充 Linux VM 手工运行方式、配置要点、接口验证和常见故障排查。
   - 已新增 Docker Compose 依赖场景下的服务配置样例。
   - 已新增服务容器内使用的配置样例。
   - 已补充 Nginx 场景下的本地运行说明。
   - 避免把本机临时配置写死到代码中。
3. Nginx：
   - 状态：已完成。
   - 已提供统一 HTTP 入口和反向代理配置。
   - 已验证短链创建和跳转路径经 Nginx 转发仍然正常。
4. 文档与验证：
   - 状态：已完成。
   - 已更新部署文档和运行手册。
   - 已完成 v1.2 本地干净克隆 Compose 验证。

## 阶段 5：测试与 CI

计划能力：

- 框架测试。
- API 测试。
- MySQL / Redis 集成测试。
- CI 构建验证。

当前状态：已完成。已建立最小测试骨架并补充第一批框架基础测试、短链业务纯逻辑测试、API 冒烟测试脚本、MySQL / Redis 集成测试脚本、Redis 不可用回退测试脚本和 Compose 依赖编排脚本；CI 第一版 workflow 已新增，核心命令链路已在 Linux VM 中验证，GitHub Actions 云端 CI 已通过。

### v1.3 执行批次

v1.3 目标是把当前手工验证沉淀为可重复执行的测试体系，已完成第一版收口。

1. 最小测试骨架：
   - 状态：已完成。
   - 新增测试目录和最小测试可执行程序。
   - 接入 CMake / CTest 测试入口。
2. 框架测试：
   - 状态：已完成第一批基础测试。
   - 覆盖路由匹配、路径参数、请求解析、响应构造和中间件顺序。
3. 短链业务纯逻辑测试：
   - 状态：已完成第一批基础测试。
   - 覆盖 URL 校验、内存 repository 创建和查询、短码不存在等场景。
4. API 测试：
   - 状态：已完成第一批 API 冒烟测试脚本。
   - 覆盖健康检查、创建短链接、短码跳转、错误响应。
5. 集成测试：
   - 状态：已完成第一批 MySQL / Redis 依赖已启动版脚本和 Compose 依赖编排脚本。
   - 覆盖 MySQL 持久化、Redis 未命中回源、Redis 回填和 Redis 不可用回退 MySQL。
6. CI：
   - 状态：已完成第一版 workflow，核心命令链路已在 Linux VM 中验证，GitHub Actions 云端 CI 已通过。
   - 第一版在远端 Linux 环境中自动执行构建、CTest 和 API 冒烟测试。
   - MySQL / Redis Compose 集成测试后续作为增强项单独接入。

## 阶段 6：性能与稳定性

计划能力：

- 压测。
- Redis 连接池或连接复用评估。
- MySQL 连接池参数验证。
- 错误场景覆盖。
- Nginx 入口底线验证。

当前状态：已完成。v1.4 已建立可重复的 curl / `hey` 压测基线，修复 MySQL 创建路径大小写短码冲突，完成异常场景、Redis 环境问题定性、Nginx 入口底线和全量回归验证；连接池和限流实现不作为 v1.4 目标。

### v1.4 执行批次

v1.4 目标是在已有测试基础上观察服务在压力和异常场景下的表现。当前阶段先建立可重复压测入口和基线数据，再基于证据决定是否调整 Redis、MySQL 或框架层资源管理。

1. v1.4.0 文档定界：
   - 状态：已完成。
   - 明确 v1.4 范围、非目标、执行顺序和验收标准。
   - 确认不提前声明性能指标，不把 v1.5 可观测性能力纳入 v1.4。
2. v1.4.1 压测脚本与基线格式：
   - 状态：已完成。
   - 新增 `tests/scripts/benchmark_shortlink.sh` 作为可重复执行的压测入口。
   - 支持健康检查、创建短链、短码跳转、非法 URL 和不存在短码场景。
   - 支持内存存储、MySQL 持久化、MySQL + Redis 查询缓存三种模式的配置入口。
   - 明确压测环境、工具、接口、并发参数和结果记录格式。
3. v1.4.2 多模式压测基线：
   - 状态：已完成第一轮 curl fallback 基线。
   - 已覆盖内存存储、MySQL 持久化、MySQL + Redis 查询缓存三种模式。
   - 已记录 QPS、平均延迟、P95、P99 和错误率。
   - 已发现 MySQL 创建短链并发错误率较高、Redis 查询缓存路径延迟异常偏高，需后续排查。
4. v1.4.3 MySQL 创建路径并发失败诊断：
   - 状态：已完成。
   - 针对 BUG-004 补充 `tests/scripts/mysql_create_concurrency_diagnostic.sh`，保留状态码分布、响应体样本和服务日志。
   - 已按并发 1、2、4、8、16 验证 `POST /api/short-links`；并发 1 已可复现 500。
   - 根因是 Base62 短码大小写敏感，但 MySQL `code` 唯一索引使用大小写不敏感排序规则。
   - 已将 `short_links.code` 调整为 `utf8mb4_bin`，并新增已有表迁移脚本。
   - 修复后阶梯诊断各档位均为 `201 100`；`mysql create` 和 `mysql-redis create` 错误率均回到 0.00%。
5. v1.4.4 异常场景补强：
   - 状态：已完成。
   - 覆盖 Redis 不可用、MySQL 不可用、非法请求高并发、空 body / 非 JSON / 缺少 `url` 和短码不存在高并发。
   - 新增 `tests/scripts/shortlink_exception_scenarios_test.sh`，保留 summary、临时配置、服务日志、响应体和状态码分布。
   - 首轮验证中，Redis 不可用可回源 MySQL；MySQL 不可用时服务在 ready 前退出；请求异常场景返回稳定 4xx。
   - 本批次未发现新的非预期 500、超时或进程挂死；连接优化进入 v1.4.5。
6. v1.4.5 Redis 环境问题定性与配置策略验证：
   - 状态：已完成。
   - 优先排查 Redis hit 和 missing-code 路径异常慢的问题，先定位服务内 Redis cache 路径固定延迟来源。
   - 新增 `tests/scripts/redis_cache_diagnostic.sh`，对比 Redis 直连命令、HTTP Redis hit / miss、missing-code 和纯 MySQL 查询路径。
   - 首轮诊断显示 Redis 直连命令和新建连接均为亚毫秒级，但 HTTP Redis hit 约 0.207s，Redis miss 回填约 0.416s，慢点集中在服务内 Redis cache 路径。
   - 新增 `tests/scripts/redis_hiredis_segment_diagnostic.sh`，分段测量 `resolve`、`redisConnectWithTimeout`、`GET` / `SETEX` / `PING`、`redisFree`，并增加 `TCP_NODELAY` 对照。
   - 分段诊断显示 `resolve`、`connect`、`free` 均为亚毫秒级；使用 `docker.orb.internal` 或显式 IPv4 时慢点集中在 hiredis 命令阶段，约 0.208s；使用 IPv6 字面地址时 HTTP Redis hit 降至约 0.0003s。
   - 将该现象定性为本地 OrbStack VM 访问 Docker Redis 发布端口的环境特定网络路径问题，不作为业务代码层面的 Redis bug。
   - 已验证本地 VM IPv6 地址策略：HTTP Redis hit 约 0.000304s，Redis miss 回源回填约 0.000769s，missing-code 约 0.000633s，错误率均为 0.00%。
   - Redis 不可用 fallback 和异常场景脚本均通过，地址策略不改变业务语义。
   - 容器内运行时仍优先通过 Compose 服务名 `redis:6379` 访问 Redis，不使用本地 VM IPv6 地址规避。
   - 当前不实现 Redis 连接池；该 0.2s 固定延迟不归因为连接池缺失。
   - MySQL 连接池参数、worker 线程数量关系和连接池等待超时机制后置到后续证据更明确时再评估。
7. v1.4.6 压测工具与 Nginx 入口底线验证：
   - 状态：已完成。
   - 已在 Linux VM 安装固定版本 `hey v0.1.5`，并记录 Go 工具链和安装方式。
   - 压测脚本已改用 `hey -o csv` 的单轮原始结果计算 QPS、平均延迟、P95、P99 和错误率，避免依赖展示文本格式。
   - 已修复 302 能力检测和单请求 Redis miss 并发参数，跳转场景可稳定校验预期状态码。
   - 已增加 `HAOHTTP_BENCH_BASE_URL` 外部目标入口，同一脚本可验证已有 Nginx 或服务入口。
   - 已使用 `hey` 完成直连核心小基线，health、create、redirect、Redis hit / miss、missing-code 和 invalid-url 错误率均为 0.00%。
   - 已使用当前工作区重新构建 Compose 服务镜像，并完成 Nginx IPv6 入口小基线；核心接口错误率均为 0.00%。
   - `docker.orb.internal:8080` 在并发 `hey` 下复现约 0.21s P95，IPv6 字面地址下恢复到毫秒级，归入 ENV-001 的本地网络路径范围，不作为 Nginx 或业务代码 bug。
8. v1.4.7 回归验证与版本收口：
   - 状态：已完成。
   - Linux VM 构建、CTest 1/1 和 API smoke 通过。
   - Linux VM 独立干净克隆构建、CTest 1/1 和 API smoke 通过。
   - MySQL / Redis 集成测试通过，覆盖持久化、Redis miss 回源和缓存回填。
   - Redis 不可用 fallback 通过，创建与跳转仍可使用 MySQL。
   - 异常场景回归通过，Redis 不可用、MySQL 不可用和三种存储模式下的非法请求 / 短码不存在结果稳定。
   - v1.4 不实现 Redis 连接池；MySQL 连接池参数矩阵和连接等待超时后置到出现新的资源等待证据时再评估。
   - 限流正式实现进入 v1.6，不在 v1.4 内继续扩展。

### v1.4 非目标

- 不接入 Prometheus、Grafana 或完整指标系统。
- 不实现用户系统、访问统计或消息队列。
- 不大规模重写 `HttpServer/`。
- 不在缺少压测证据时重写 Redis 或 MySQL 连接管理。
- 不声明生产性能指标或最大承载能力。
- 不用限流掩盖已暴露的创建路径稳定性问题；需先解释并处理 BUG-004。
- 不在 v1.4 实现 Redis 限流；限流作为 v1.6 流量保护阶段的正式目标。

### v1.4 验收标准

- `docs/BENCHMARK.md` 记录可复现压测环境、命令、场景和结果。
- 至少完成内存存储、MySQL 持久化、MySQL + Redis 查询缓存三组基线。
- 至少覆盖 Redis 不可用、MySQL 不可用、非法请求高并发和短码不存在高并发。
- 至少补充 Nginx 入口底线验证，确认反向代理链路仍可用。
- 明确记录是否需要 Redis 连接池、MySQL 连接池超时或参数调整。
- Linux VM 构建、CTest 和 API 冒烟测试继续通过。
- 未将 v1.5 的可观测性能力描述为已完成。

## 阶段 7：可观测性与依赖 CI

计划能力：

- 结构化日志。
- 指标统计。
- Prometheus。
- Grafana。
- MySQL / Redis 集成 CI。

当前状态：已完成。v1.5.0 设计收口、v1.5.1 request ID / 通用结构化请求日志、v1.5.2 指标实现、v1.5.3 本地 Prometheus / Grafana 展示和 v1.5.4 自动化验证 / 性能回归 / 文档收口均已完成。

### v1.5 执行批次草案

v1.5 目标是让服务运行状态可观察、可排查，并把已经稳定的 MySQL / Redis 集成回归放入远端 CI。

1. 观测边界设计：
   - 状态：已完成。
   - 明确请求日志、错误日志、关键业务日志、`request_id` 和敏感字段边界。
   - 明确指标名称、低基数标签、`/metrics` 暴露范围和线程安全要求。
   - 详细设计见 `docs/OBSERVABILITY_DESIGN.md`。
2. request ID 与请求匹配信息：
   - 状态：已完成。
   - 校验或生成 request ID，并通过响应头返回。
   - 在请求处理链中传递同一 request ID，并由 Router 返回注册时的路由模板。
3. 结构化日志：
   - 状态：已完成 v1.5 第一批通用请求日志。
   - 整理通用 HTTP 字段与短链接业务字段的框架 / 应用边界。
   - 第一批完成单行 key-value `http_request` 日志，再逐步补充业务事件。
   - 创建、跳转、缓存和后端错误业务事件随对应指标埋点继续补充。
4. 指标埋点与暴露：
   - 状态：已完成。
   - 记录请求量、错误率、延迟、创建 / 跳转结果和 Redis hit / miss / error 等基础指标。
   - 提供 Prometheus 可抓取的指标入口，避免把短码、URL、IP、User-Agent 或 `request_id` 作为指标标签。
5. 监控展示：
   - 状态：已完成。
   - 已增加 Prometheus 和 Grafana 本地编排、持久化数据卷、自动 provisioning 与六面板 dashboard。
6. 依赖 CI：
   - 状态：已完成。
   - 现有 MySQL / Redis 集成和 Redis 不可用 fallback 脚本已接入 GitHub Actions。
   - Prometheus scrape、Grafana datasource / dashboard、Nginx 暴露边界和监控数据卷恢复已作为独立 job 接入 GitHub Actions。
   - CI 使用 `127.0.0.1` 访问 Compose 发布端口，失败时输出依赖日志，结束时清理临时数据卷。
   - CI 暂不执行性能基线，也不把本地 OrbStack 地址策略带入 GitHub runner。
7. v1.5.4 收口：
   - 状态：已完成。
   - 已在同一 Linux VM、相同脚本和参数下对比 v1.4.0 与 v1.5 代表性 `hey` 小基线。
   - 已记录可测量的观测开销、测试波动和非生产结论边界，未据此提前实施连接或日志架构优化。
   - 已对齐 README、Roadmap、测试计划、压测记录和可观测性设计的完成状态。

## 阶段 8：可靠性与流量保护

计划能力：

- Redis 限流。
- 限流配置。
- 降级策略。
- 保护性测试。
- 健康检查语义。

当前状态：已完成。v1.6.0-v1.6.6 实现、本地回归、干净克隆和 GitHub Actions 云端 CI 验证均已完成。

### v1.6 执行批次

v1.6 目标是基于 v1.5 的日志和指标补充基础流量保护与依赖降级语义。该阶段优先引入 Redis
作为限流状态存储，避免只写进程内限流后无法覆盖多实例场景。

1. v1.6.0 设计与边界收口：
   - 状态：已完成。
   - 第一版只保护 `POST /api/short-links`，使用跨实例共享的全局创建额度。
   - 选择 Redis Lua 固定窗口，超限返回 `429` 和 `Retry-After`。
   - Redis 限流故障采用 fail-open，不改变 MySQL 事实存储语义。
   - 按 IP 限流后置到客户端身份和可信代理边界明确之后。
   - 详细设计见 `docs/RELIABILITY_DESIGN.md`。
2. v1.6.1 健康与降级语义：
   - 状态：已完成。
   - 保留 `/api/health` 兼容入口，增加 liveness 和 readiness。
   - MySQL 是持久化模式的 readiness 必要依赖；Redis 查询缓存和限流均为可降级依赖。
   - 依赖探测必须有界，不得无限等待连接池。
3. v1.6.2 限流领域接口：
   - 状态：已完成。
   - 建立与 Redis 解耦的 allowed / limited / error 结果和可测试抽象。
   - 先验证 HTTP 处理分支，不让路由直接依赖 hiredis 细节。
4. v1.6.3 Redis 固定窗口实现：
   - 状态：已完成。
   - 使用 Lua 原子完成计数、过期和 TTL 返回。
   - 限流开关与查询缓存开关独立，复用现有 Redis 连接配置。
5. v1.6.4 HTTP、配置和可观测性接入：
   - 状态：已完成。
   - 超限请求稳定返回统一 JSON 错误、`429` 和 `Retry-After`。
   - 记录 allowed / limited / error 低基数指标和 Redis fail-open 日志。
6. v1.6.5 保护性测试与 CI：
   - 状态：已完成，GitHub Actions 云端 CI 已通过。
   - 覆盖关闭兼容、窗口内放行、超限、过期恢复、并发、Redis 不可用和健康语义。
   - 覆盖内存 + 限流和 MySQL 关闭查询缓存 + 限流。
   - 将稳定的 Redis 保护性测试接入现有依赖 CI job。
7. v1.6.6 回归与版本收口：
   - 状态：已完成。
   - 执行 Linux VM 全量回归、Nginx 入口验证、代表性小基线和干净克隆验证。
   - 对齐 README、API、Runbook、Test Plan、Decisions 和设计文档状态。
   - Linux VM 干净克隆：`/tmp/haoHTTP-v1.6-clean-ced354a`；构建目录：`/tmp/haoHTTP-v1.6-build-ced354a`。
   - Docker 宿主侧干净克隆：`/tmp/haoHTTP-v1.6-host-ced354a`。

### v1.6 非目标

- 按 IP、用户、API key 或短码限流。
- 可信代理和客户端真实 IP 解析。
- 限制短码跳转路径。
- 滑动窗口、令牌桶、Nginx `limit_req` 或多级限流。
- Redis 连接池重构。
- 自动封禁、黑名单、风控、身份系统和生产告警。

## 阶段 9：链接生命周期

计划能力：

- 链接状态。
- 过期策略。
- 链接详情与列表。
- 缓存失效。

当前状态：已实现并完成本地回归与 GitHub Actions 云端 CI，具体语义见 `docs/LIFECYCLE_DESIGN.md`。

### v1.7 执行批次草案

v1.7 目标是在引入消息队列前补齐短链接自身的基础生命周期，使业务层不再只有创建和跳转。

1. 数据模型与迁移：
   - 状态：已完成。
   - 增加状态和过期时间字段，并明确已有数据的迁移与默认值。
2. 跳转语义：
   - 状态：已完成。
   - 禁用或过期短链不再跳转，错误响应和状态码需明确。
3. 管理接口：
   - 状态：已完成。
   - 提供基础详情、列表和禁用能力；删除优先使用软删除或状态变更。
4. 缓存一致性：
   - 状态：已完成。
   - 状态、过期时间变更后正确删除或更新 Redis 缓存。
5. 生命周期测试：
   - 状态：已完成本地回归与 GitHub Actions 云端 CI。
   - 覆盖有效、禁用、过期、缓存旧值和迁移后数据等场景。

## 阶段 10：消息队列与异步事件

计划能力：

- Kafka。
- Kafka UI。
- 版本化访问事件和异步 producer。
- 独立 consumer、consumer group 和手动 offset。
- 有界队列、delivery callback、fail-open 和低基数观测。
- Broker 中断、队列满、重复、非法事件和消费者重启验证。

当前状态：已完成。v1.8.0-v1.8.6 本地实现、全量回归、干净目录验证和 GitHub Actions 云端 CI 均已通过。详细设计见 `docs/ACCESS_EVENT_DESIGN.md`。

### v1.8 执行批次

v1.8 目标是引入 Kafka，将短码跳转产生的访问事件从同步请求路径中拆出，为后续访问统计提供版本化、
可重放的事件来源。该阶段采用已经确认的完整工程规划；RabbitMQ 仍只作为未来任务队列候选。

1. `v1.8.0` 设计与依赖验证：
   - 状态：已完成。
   - 已固化事件 schema、topic、生产 / 消费职责、丢失与重复边界、隐私字段和后续升级路径。
   - 在 Linux VM 验证 librdkafka、目标 Kafka 镜像、CMake 和 Docker 依赖链。
2. `v1.8.1` 事件契约与抽象：
   - 状态：已完成。
   - 实现 AccessEvent、JSON codec、独立 event ID、publisher 接口、Noop / fake 和单元测试。
3. `v1.8.2` 异步 producer：
   - 状态：已完成。
   - 在应用 handler 边界异步发布 success、not_found、disabled、expired 和 error 事件。
   - 实现有界队列、poll、delivery callback、idempotent producer、指标、有界关闭和 fail-open。
   - Kafka 不参与 readiness，初始化失败、broker 故障或队列满不能改变跳转 HTTP 结果。
4. `v1.8.3` Kafka 本地编排：
   - 状态：已完成。
   - 使用独立 Compose overlay 增加单节点 KRaft Kafka、显式 topic 初始化和本地 Kafka UI。
   - 固定镜像版本，补充容器 / VM listener、数据保留和排障说明。
5. `v1.8.4` 独立 consumer：
   - 状态：已完成。
   - 新增 `shortlink_event_consumer`，实现 consumer group、schema 校验、手动 offset、错误退避、discard 和有界关闭。
   - v1.8 只记录有效事件，不提前实现统计写入、业务幂等表、重试 topic 或 DLQ。
6. `v1.8.5` 故障与可观测性收口：
   - 状态：已完成本地验证，独立 Kafka CI job 已配置。
   - 覆盖 broker 启停、队列满、delivery failure、重复、非法事件、consumer 重启和 shutdown。
   - 增加低基数 producer / consumer 观测，并接入适合的 Kafka 集成 CI。
7. `v1.8.6` 全量回归与文档收口：
   - 状态：已完成；GitHub Actions 云端 CI 已通过。
   - 比较 Kafka 关闭、正常和故障三组 redirect 基线。
   - Linux VM、完整 Compose、干净源码目录、Ubuntu 22.04 Docker 构建和云端 Kafka job 均已通过。

## 阶段 11：访问统计与工程化增强

计划能力：

- 访问统计聚合。
- 统计表。
- 统计查询 API。
- 消费幂等、重试和 DLQ。
- Consumer lag、异步一致性和重放验证。
- 配置分层。
- 发布检查。

当前状态：已完成。本地实现、完整 Compose 故障回归、独立干净目录和 GitHub Actions run `29679640891` 均已通过。详细边界见 `docs/ACCESS_STATISTICS_DESIGN.md`。

### v1.9 执行批次

v1.9 目标是将 v1.8 的访问事件落成可查询的业务能力，并把已经引入的依赖整理为更稳定的运行方式。

1. `v1.9.0` 设计与迁移契约：
   - 状态：已完成并由实现回归验证。
   - 固化统计语义、内部 API、MySQL 投影、幂等事务、失败分类、DLQ 和重放边界。
2. `v1.9.1` 数据模型与 MySQL 幂等投影：
   - 状态：已完成本地验证。
   - 新增消费收据、按短链 / 结果累计和 UTC 小时趋势表，覆盖空库与 v1.8 原地升级。
3. `v1.9.2` consumer 统计写入：
   - 状态：已完成本地验证。
   - 使用 MySQL 事务处理 `event_id` 和统计更新，业务提交成功后再提交 Kafka offset。
4. `v1.9.3` 查询接口：
   - 状态：已完成本地验证。
   - 提供成功访问次数、尝试次数、最近访问时间、结果分类和小时 / 天趋势。
5. `v1.9.4` 失败处理与观测：
   - 状态：已完成本地故障验证。
   - MySQL 临时错误退避重试；永久非法或不兼容消息进入 DLQ；增加 consumer lag 和低基数指标。
6. `v1.9.5` 一致性与重放验证：
   - 状态：已完成本地重放与隔离重建验证。
   - 覆盖重复消息、消费重启、延迟可见、consumer lag、幂等重放和隔离统计重建。
7. `v1.9.6` 工程化与发布收口：
   - 状态：已完成；配置、Compose、CI 入口、运行手册、独立干净目录和云端 CI 均已通过。
   - 将 Kafka 和统计链路纳入适合的集成 CI，区分本地、测试和容器内配置。
   - 形成构建、测试、迁移、回滚和文档检查清单，并完成全量与干净目录验证。

## 阶段 12：产品闭环与应用层收口

计划能力：

- 用户系统。
- 链接归属。
- 权限管理。
- 自定义短码。
- 管理 API 和最小管理页面。
- OpenAPI 契约、迁移与安全验收。

当前状态：已实现并完成本地功能、迁移、依赖、故障和浏览器验收；远端交付状态以对应 Pull Request
检查记录为准。

### v2.0 执行批次

v2.0 是第一个“可以被正常使用和演示”的短链接服务里程碑。链接生命周期和访问统计已在前序版本落地；
该版本不追求复杂社交登录、计费或生产级高可用，重点是补齐产品入口、安全边界和可维护的应用层结构。

1. `v2.0.0` 契约与应用层边界：
   - 状态：已完成。
   - 冻结身份、会话、链接归属、对象级权限和管理 API 语义。
   - 拆分当前过大的应用入口与 handler / service 组合职责，保持 C++17 和现有 `HttpServer/` 稳定。
2. `v2.0.1` 数据迁移：
   - 状态：已完成。
   - 引入轻量 schema migration 记录，增加用户、会话和链接 owner 字段。
   - 同时验证空数据库初始化、v1.9 数据升级、旧链接归属回填和失败回滚。
3. `v2.0.2` 身份与会话：
   - 状态：已完成。
   - 提供注册、登录、退出和当前用户查询；密码使用适合密码存储的哈希算法。
   - 会话 token 必须足够随机、服务端只保存不可逆摘要，并具备过期和撤销语义；日志和指标不得泄露凭据。
4. `v2.0.3` 链接归属与权限：
   - 状态：已完成。
   - 创建、列表、详情、更新、禁用 / 恢复和统计查询接入 owner 与对象级授权。
   - 公共短码跳转不要求登录；管理权限必须显式建模，不能只依赖隐藏路径。
5. `v2.0.4` 产品入口：
   - 状态：已完成。
   - 提供最小管理页面，至少支持登录、创建、列表、禁用 / 恢复、过期设置和统计查看。
   - 提供机器可读 OpenAPI 文档，并保证示例请求可以实际运行。
   - 支持自定义短码，明确字符集、长度、大小写、保留路径、唯一约束、并发冲突和归属权限。
6. `v2.0.5` 验收：
   - 状态：已完成本地验收；详见 `docs/V2_ACCEPTANCE.md`。
   - 覆盖身份、会话、对象越权、输入边界、迁移、浏览器端到端、依赖降级和旧能力回归。
   - 管理页面和 API 必须形成可重复演示闭环，而不是只证明若干内部线程或组件能够启动。

v2.0 非目标：Go 服务、OIDC、邮件、计费、自定义域名、复杂前端框架、独立分析平台和多租户配额系统。

## 阶段 13：Kubernetes 部署与多副本验收

当前状态：草案，尚未实现。

v2.1 目标是把已经容器化并具备健康检查、指标和外部事实存储的应用部署到 Kubernetes，验证配置注入、
多副本、滚动发布和故障恢复。第一版不把生产级有状态集群运维作为验收条件。

1. `v2.1.0` 环境与部署边界：
   - 固定一种本地可重复 Kubernetes 环境，并记录工具版本与主机资源。
   - 为 `shortlink_server`、`shortlink_event_consumer` 和管理入口提供应用工作负载清单。
   - 仅在 MySQL 持久化模式下验证服务多副本，不把进程内存模式作为扩容方案。
   - MySQL、Redis 和 Kafka 第一版允许使用外部依赖或仅供本地验收的单节点部署，不声明生产高可用。
2. `v2.1.1` 镜像、工作负载与网络：
   - 为服务、消费者和管理入口提供可复现镜像、Deployment 与 Service。
   - 使用 ConfigMap 和 Secret 分离普通配置与凭据。
   - 提供 Service，并选择 Ingress 或复用 Nginx 形成受控入口；继续保护 `/internal/` 和 `/metrics`。
3. `v2.1.2` 健康、资源与观测：
   - 接入现有 liveness / readiness 和 consumer 健康入口。
   - 设置可解释的 CPU / 内存 requests 与 limits；HPA 只在获得负载和扩缩容证据后评估。
   - 保持 Prometheus 指标可抓取，避免将内部管理与指标端口直接公开。
4. `v2.1.3` 发布与故障验证：
   - 覆盖滚动升级、回滚、Pod 重建、服务多副本、Kafka consumer group 扩展和关键依赖故障。
   - 验证 MySQL 事实存储、Redis 全局限流和 Kafka 分区 / consumer group 在多副本下的既有语义。
5. `v2.1.4` 可重复演示入口：
   - 提供一组明确的启动、健康检查、演示和清理命令，并在干净环境中验证。
   - 记录本地验收与 CI 的边界；性能和长稳测试不作为每次 PR 的硬门禁。
6. `v2.1.5` 验收：
   - 执行多副本功能回归、发布 / 回滚、Pod 故障、依赖故障和 Kubernetes 场景性能对照。

v2.1 非目标：生产级 MySQL / Redis / Kafka StatefulSet、多可用区高可用、Terraform、公有云托管、
Service Mesh、Operator。Helm 和 HPA 仅在基础清单稳定且有重复部署或负载证据时评估。

## 阶段 14：生命周期事件可靠发布

当前状态：草案，尚未实现。

v2.2 目标是使用 Transactional Outbox 可靠发布链接生命周期事件，解决 MySQL 业务变更与 Kafka 发布之间
的双写窗口，并让事件驱动一个可见、可查询的生命周期审计投影。该版本同时承担阶段性终点的全量验收与发布收口。

1. `v2.2.0` 生命周期事件契约：
   - 覆盖链接创建、禁用、恢复、过期时间和归属变更等本来就写 MySQL 的操作。
   - 固化版本化 schema、`event_id`、聚合对象、事件类型、分区键、顺序、隐私和保留边界。
   - 访问事件继续来自读路径，不为每次跳转增加同步 outbox 写入。
2. `v2.2.1` 原子写入：
   - 在同一 MySQL 事务中写业务表和 outbox 表，并通过 migration 管理表结构。
   - 明确待发布、重试、已发布或终止状态、尝试次数、下次重试时间和清理所需字段。
3. `v2.2.2` relay：
   - 实现独立 `shortlink_outbox_relay` 或等价进程，负责并发安全领取、Kafka 发布、退避重试、优雅停止和清理。
   - relay 发布成功但状态回写失败时允许重复发送，下游继续使用 `event_id` 幂等。
4. `v2.2.3` 真实下游：
   - 使用独立职责的生命周期 consumer 将事件幂等投影为可查询审计记录，并在管理 API / 页面中展示时间线。
   - 不为了展示 Outbox 而制造无消费者 topic，也不把生命周期职责含混地塞进入口服务。
5. `v2.2.4` 观测与恢复：
   - 覆盖业务事务回滚、Kafka 中断、relay 崩溃窗口、重复发布、consumer 重启、积压、恢复和历史清理。
   - 增加低基数发布结果、积压量、最老待发布事件年龄和消费结果指标，不声明端到端 exactly-once。
6. `v2.2.5` 阶段终验与发布：
   - 执行功能、安全、迁移、浏览器端到端、Kubernetes、多副本、故障注入、性能、长稳和干净环境回归。
   - 更新 API、架构、运行、部署、测试、压测和故障处理文档，形成可重复演示入口。
   - 达到 `docs/FINAL_ACCEPTANCE.md` 的完成定义后发布 v2.2.5，并冻结阶段性功能范围，转入维护、演示和缺陷修复。

## 阶段终点之后的条件 backlog

以下能力没有预先分配版本号，也不是 v2.0-v2.2 的验收条件：

- 只有出现多个跨语言消费者或频繁 schema 演进后，才评估 Schema Registry、Avro、Protobuf 或 JSON Schema。
- 只有 Kafka 无法覆盖新的投递语义且存在明确消费者后，才评估 RabbitMQ；不为重复展示消息队列而引入。
- 只有出现全文检索、复杂聚合或大规模分析数据量后，才评估 Elasticsearch 或 ClickHouse。
- Go、多云部署、Terraform、Service Mesh、自定义域名、OIDC 和计费继续等待独立需求，不作为阶段终点的“技术栈补全”。
