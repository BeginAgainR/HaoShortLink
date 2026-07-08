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

## 待决策

### API 版本

状态：待决策

当前暂不引入 `/api/v1`。后续如果需要兼容多个 API 版本，再重新评估。

## 暂缓

以下能力不属于当前阶段：

- 用户系统。
- 访问统计。
- Redis 限流。
- 完整线上部署。
- HTTPS/TLS 终止。
- 发布回滚。
- 消息队列。
- 监控和告警。
