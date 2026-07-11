# 已知问题

状态：持续维护
记录范围：只记录已经确认存在，或有明确证据的问题。

## 记录模板

```text
ID：
状态：
范围：
描述：
影响：
下一步：
```

## BUG-001：旧 404 文件路径残留

状态：已修复
范围：`HttpServer/`

描述：

`HttpServer/include/utils/FileUtil.h` 中曾存在旧五子棋项目的 404 文件路径。

影响：

默认 404 文件加载可能指向已经删除的旧业务资源。

下一步：

已移除旧业务绝对路径。`FileUtil::resetDefaultFile()` 现在支持传入默认文件路径；未配置默认路径时不再回退到旧业务资源。

## BUG-002：动态路由回调未传递路径参数

状态：已修复
范围：`HttpServer/`

描述：

`Router::route()` 在动态路由 callback 匹配成功后构造了带路径参数的 `newReq`，但实际调用
callback 时传入的是原始 `req`。

影响：

使用 `addRoute()` 注册动态路由 callback 时，处理函数无法通过 `getPathParameters()` 读取路径参数。
短码跳转接口 `GET /s/{code}` 需要读取短码参数，因此会受影响。

下一步：

已改为调用 `callback(newReq, resp)`，动态路由 callback 可以读取 `param1` 等路径参数。

## BUG-003：请求重置时未清理请求体

状态：已修复
范围：`HttpServer/`

描述：

`HttpRequest::swap()` 未交换 `content_` 和 `contentLength_`。`HttpContext::reset()` 依赖
`HttpRequest::swap()` 清理当前请求对象，因此在 keep-alive 连接复用场景下，请求体状态可能残留。

影响：

后续请求可能意外保留上一条请求的 body 或 content length，影响 POST 接口和请求解析状态的可靠性。

下一步：

已在 `HttpRequest::swap()` 中补充交换 `content_` 和 `contentLength_`。

## BUG-004：MySQL 创建短链并发压测存在非 201 响应

状态：已修复
范围：`apps/shortlink_server/sql/`、`apps/shortlink_server/` MySQL 短码生成与数据模型

描述：

v1.4.2 第一轮 curl fallback 基线中，`POST /api/short-links` 在 `storage.type=mysql`、
`server.thread_num=4`、`mysql.pool_size=4`、并发 16、总请求 500 的条件下出现 41.40%
非 201 响应；`storage.type=mysql` 且 `redis.enabled=true` 时同类创建场景出现 40.40% 非 201 响应。
内存模式同场景错误率为 0。

v1.4.3 阶梯诊断结果：

- 诊断入口：`tests/scripts/mysql_create_concurrency_diagnostic.sh`
- 诊断 commit：`51dca81`
- 请求数：每档 100
- 并发档位：1、2、4、8、16
- `server.thread_num=4`
- `mysql.pool_size=4`
- artifact：`/tmp/haohttp-create-diagnostic.96ByrP`

状态码分布：

| 并发 | 201 | 500 | 错误率 |
| --- | ---: | ---: | ---: |
| 1 | 70 | 30 | 30.00% |
| 2 | 64 | 36 | 36.00% |
| 4 | 58 | 42 | 42.00% |
| 8 | 50 | 50 | 50.00% |
| 16 | 72 | 28 | 28.00% |

服务日志显示失败原因为 MySQL 唯一键冲突，例如：

```text
Duplicate entry '0000Gw' for key 'short_links.uk_short_links_code'
```

根因判断：

`short_links.code` 使用 `VARCHAR(32)` 且表默认排序规则为 `utf8mb4_0900_ai_ci`，该排序规则大小写不敏感。
当前短码生成使用大小写敏感的 Base62 字符集，因此 `0000GW` 和 `0000Gw` 在业务上是两个短码，
但在 MySQL 唯一索引中会被视为相同值，导致回写 `code` 时触发唯一键冲突并返回 500。

影响：

MySQL 持久化创建路径在短码进入大小写相邻区间后会稳定出现创建失败；该问题不只属于高并发问题，
并发 1 的连续创建也可复现。

下一步：

已修复数据模型，使 MySQL 唯一索引和业务短码比较规则一致：

- `apps/shortlink_server/sql/001_create_short_links.sql` 中 `code` 列显式使用 `utf8mb4_bin`。
- 新增 `apps/shortlink_server/sql/002_make_short_link_code_case_sensitive.sql`，用于迁移已存在的表。
- `compose.yaml` 改为挂载整个 `apps/shortlink_server/sql/` 目录，使新数据卷初始化时按顺序执行 SQL 脚本。

复测结果：

- `mysql_create_concurrency_diagnostic.sh`：并发 1、2、4、8、16 每档 100 请求，全部为 `201 100`。
- `mysql create`：并发 16、总请求 1000，错误率 0.00%。
- `mysql-redis create`：并发 16、总请求 1000，错误率 0.00%。

## ENV-001：OrbStack VM 访问 Docker 发布端口存在 IPv4 慢路径

状态：配置规避已验证
范围：本地 OrbStack VM 访问 Docker 发布端口的 Redis 和 Nginx 验证场景

描述：

v1.4.5 首轮诊断中，`shortlink_server` 在 `haoHTTP` VM 中手工运行、Redis 在 OrbStack Docker 中运行、
服务通过 `docker.orb.internal:16379` 访问 Redis 时，`redis.enabled=true` 的 HTTP Redis hit 约 0.207s，
Redis miss 回源并回填约 0.416s，而纯 MySQL 查询路径约 0.0004s。

v1.4.5.2 分段诊断新增 `tests/scripts/redis_hiredis_segment_diagnostic.sh` 后确认：

- `resolve`、`redisConnectWithTimeout`、`redisFree` 均为亚毫秒级。
- 使用 `docker.orb.internal:16379` 或显式 IPv4 `192.168.139.2:16379` 时，hiredis 的 `PING`、`GET`、`SETEX` 命令阶段均约 0.208s。
- `TCP_NODELAY` 对照不能消除该等待。
- 使用解析出的 IPv6 字面地址 `fd07:b51a:cc66::2:16379` 时，hiredis 新建连接命令恢复到亚毫秒级。
- 同样使用 IPv6 字面地址启动 HTTP 诊断后，HTTP Redis hit 从约 0.207s 降至约 0.0003s。

影响：

在当前 OrbStack VM 本地验证环境中，如果 VM 中的服务通过 `docker.orb.internal` 慢路径或显式 IPv4 访问 Docker Redis 发布端口，
所有 Redis cache 命中、未命中和回填场景都会被额外增加约 0.2s 的固定等待；Redis miss 回源回填通常会付出两次该成本。
该问题不代表 Redis 服务、业务代码或容器内 Compose 网络必然存在同类问题。

v1.4.6 Nginx 入口基线进一步确认，同类地址路径差异也会影响 VM 到 Docker Nginx 发布端口：

- 使用 `docker.orb.internal:8080` 并发压健康检查时，P95 约 0.2095s。
- 使用当前环境 IPv6 字面地址访问同一 Nginx 端口时，健康检查 P95 约 0.0025s。
- IPv6 入口下 health、create、redirect、Redis hit / miss、invalid-url 和 missing-code 错误率均为 0.00%。

因此 ENV-001 的范围不只包括 hiredis；在当前 OrbStack VM 到 Docker 发布端口的并发验证中，客户端访问地址选择也必须记录。

下一步：

已验证本地 VM 配置使用 IPv6 字面地址可以规避当前约 0.2s 固定延迟：

- hiredis GET hit new connection：平均约 0.000068s。
- HTTP Redis hit：平均约 0.000304s。
- HTTP Redis miss with MySQL backfill：平均约 0.000769s。
- HTTP mysql-redis missing-code：平均约 0.000633s。

该策略属于本地 OrbStack VM 访问 Docker 发布端口的地址路径规避，不是业务代码层面的 Redis 修复；
容器内 `shortlink_server` 仍应优先通过 Compose 服务名访问 Redis。
后续如果更换部署机器、Docker 网络或运行方式，需要重新验证 Redis 访问路径。
当前不直接把该环境问题归因为缺少 Redis 连接池。

## 不记录的内容

- 没有复现或证据的猜测。
- 尚未实现的功能。
- 纯需求变更。
- 临时学习笔记。
