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

状态：待排查
范围：`apps/shortlink_server/`、`HttpServer/` 数据库连接池

描述：

v1.4.2 第一轮 curl fallback 基线中，`POST /api/short-links` 在 `storage.type=mysql`、
`server.thread_num=4`、`mysql.pool_size=4`、并发 16、总请求 500 的条件下出现 41.40%
非 201 响应；`storage.type=mysql` 且 `redis.enabled=true` 时同类创建场景出现 40.40% 非 201 响应。
内存模式同场景错误率为 0。

影响：

MySQL 持久化创建路径在并发场景下可能不稳定，影响短链创建接口的可靠性。当前尚未确认具体失败状态码、
日志原因或根因。

下一步：

已新增 `tests/scripts/mysql_create_concurrency_diagnostic.sh`，用于保留日志和状态码分布；下一步执行该脚本，
按并发 1、2、4、8、16 逐步验证失败边界，并确认失败响应类型。随后排查 MySQL repository 事务逻辑、
数据库连接池等待/复用行为和 HTTP 请求体解析在并发 POST 下的稳定性。修复后需复跑 `mysql create` 和
`mysql-redis create` 基线，目标是错误率回到 0。

## 不记录的内容

- 没有复现或证据的猜测。
- 尚未实现的功能。
- 纯需求变更。
- 临时学习笔记。
