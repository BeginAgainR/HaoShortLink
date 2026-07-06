# 运行手册

状态：草案
当前实现：已整理 Linux VM 手工构建、配置和运行方式；当前阶段不在 Mac 宿主机上构建或运行。

## 本地 Mac 宿主机

当前 Mac 宿主机仅用于：

- 编辑代码和文档。
- 查看 git 状态和 diff。
- 维护项目文档。

当前 Mac 宿主机不用于：

- 构建项目。
- 运行服务。
- 生成 `build/` 目录。
- 执行压测。

## Linux 验证环境

构建和运行验证后续在 Linux 虚拟机或容器环境中进行。

当前本地验证环境：

- OrbStack Linux VM：`haoHTTP`
- 进入方式：`ssh haoHTTP@orb`
- 项目路径：`/Users/hao/Code/haoHTTP`
- 当前分支：`refactor/hao-shortlink-cleanup`

当前 VM 已安装：

- 基础构建工具：`build-essential`、`cmake`、`git`、`pkg-config`
- 开发依赖：`libssl-dev`、`libmysqlclient-dev`、`libmysqlcppconn-dev`、`libhiredis-dev`
- muduo 核心库：`muduo_base`、`muduo_net`

本地 muduo 安装位置：

```text
/opt/muduo-src
/usr/local/include/muduo
/usr/local/lib/libmuduo_base.a
/usr/local/lib/libmuduo_net.a
```

说明：

- HaoShortLink 当前只依赖 `muduo_base` 和 `muduo_net`。
- muduo 自带的可选 `muduo_http` 组件不是当前项目依赖。
- muduo 全量构建在 GCC 15 下可能因 `-Werror` 将警告提升为错误；当前只安装项目需要的核心库。

## 开发与验证工作流

日常开发采用：

```text
Mac 本地编辑代码和管理 git
OrbStack Linux VM 构建、运行和验证
```

说明：

- 项目目录由 VM 直接访问 Mac 上的工作区。
- 构建命令必须在 Linux VM 中执行。
- Mac 宿主机只负责编辑、查看 diff 和提交。
- 构建产物不得提交到仓库。
- 日常构建目录使用 `/tmp/haoHTTP-build`，不要在仓库内生成 `build/`。

## 验证方式

### 日常挂载验证

日常开发优先使用挂载目录验证：

```text
ssh haoHTTP@orb
cd /Users/hao/Code/haoHTTP
```

这种方式适合频繁迭代，可以直接验证当前工作区中的改动。

### 干净克隆验证

阶段性验证可以使用从远端仓库重新克隆的方式。

适用场景：

- 每个版本节点完成前，例如 v0.2、v1.0。
- 修改 `CMakeLists.txt`、依赖、构建脚本或 `.gitignore` 后。
- 引入 MySQL、Redis、Docker Compose、Nginx 等外部组件前后。
- 准备合并分支、发布版本或做最终确认前。
- 怀疑当前工作区依赖了未提交文件时。

干净克隆验证更接近 CI，因为它从远端仓库拉取代码，可以发现漏提交文件、本地隐藏依赖和
`.gitignore` 配置问题。

## 配置文件

当前配置加载使用简单 `key=value` 文本格式。

示例文件：

```text
apps/shortlink_server/config/server.conf.example
```

示例内容：

```text
server.name=HaoShortLink
server.port=8080
server.thread_num=4
log.level=INFO
storage.type=memory
redis.enabled=false
```

说明：

- 空行和以 `#` 开头的整行注释会被忽略。
- key 和 value 两侧空白会被裁剪。
- 服务启动时可以传入配置文件路径；未传入时默认读取示例配置文件。
- `storage.type` 当前支持 `memory` 和 `mysql`。
- `memory` 是默认存储方式。
- `mysql` 需要先创建 `short_links` 表并配置 MySQL 连接信息。
- `redis.enabled` 是 MySQL 存储模式下的可选查询缓存开关，默认关闭。

建议：

- 本地调试时可以复制示例文件为临时配置文件，并按环境修改数据库密码等敏感信息。
- 不要把包含真实密码的个人配置提交到仓库。
- Docker Compose、Nginx 和线上配置模板尚未接入。

## 服务启动

服务二进制由 Linux VM 中的构建目录产生。日常使用挂载目录构建时，默认路径为：

```text
/tmp/haoHTTP-build/shortlink_server
```

### 内存存储模式

内存模式适合快速验证 HTTP 服务和短链 API，不依赖 MySQL 或 Redis。进程重启后短链数据会丢失。

配置要点：

```text
storage.type=memory
redis.enabled=false
```

启动命令：

```bash
ssh haoHTTP@orb
cd /Users/hao/Code/haoHTTP
/tmp/haoHTTP-build/shortlink_server apps/shortlink_server/config/server.conf.example
```

### MySQL 持久化模式

MySQL 模式用于验证短链数据持久化。MySQL 是短链数据的事实来源。

配置要点：

```text
storage.type=mysql
mysql.host=tcp://127.0.0.1:3306
mysql.user=hao_shortlink
mysql.password=change_me
mysql.database=hao_shortlink
mysql.pool_size=4
redis.enabled=false
```

数据库初始化脚本：

```text
apps/shortlink_server/sql/001_create_short_links.sql
```

初始化表结构示例：

```bash
ssh haoHTTP@orb
mysql -u hao_shortlink -p hao_shortlink < /Users/hao/Code/haoHTTP/apps/shortlink_server/sql/001_create_short_links.sql
```

启动命令：

```bash
ssh haoHTTP@orb
cd /Users/hao/Code/haoHTTP
/tmp/haoHTTP-build/shortlink_server /path/to/local-server.conf
```

### MySQL + Redis 查询缓存模式

Redis 只作为短码跳转查询缓存，不承担短链事实存储。Redis 不可用时，服务应继续回源 MySQL。

配置要点：

```text
storage.type=mysql
redis.enabled=true
redis.host=127.0.0.1
redis.port=6379
redis.database=0
redis.ttl_seconds=3600
redis.key_prefix=shortlink:
```

启动命令与 MySQL 模式相同，区别只在配置文件。

## 手工接口验证

以下命令在 Linux VM 中执行，服务默认监听 `8080` 端口。

健康检查：

```bash
curl -i http://127.0.0.1:8080/api/health
```

创建短链接：

```bash
curl -i \
  -H 'Content-Type: application/json' \
  -d '{"url":"https://example.com/very/long/path"}' \
  http://127.0.0.1:8080/api/short-links
```

短码跳转：

```bash
curl -i http://127.0.0.1:8080/s/000001
```

不存在短码：

```bash
curl -i http://127.0.0.1:8080/s/notfound
```

非法 URL：

```bash
curl -i \
  -H 'Content-Type: application/json' \
  -d '{"url":"ftp://example.com/file"}' \
  http://127.0.0.1:8080/api/short-links
```

## 常见故障排查

### 服务启动后接口无法访问

检查项：

- 服务是否在 Linux VM 中启动，而不是 Mac 宿主机。
- 配置中的 `server.port` 是否为预期端口。
- 端口是否被已有进程占用。
- curl 是否请求 `127.0.0.1` 和正确端口。

### MySQL 模式创建短链失败

检查项：

- `storage.type` 是否为 `mysql`。
- MySQL 服务是否启动。
- `mysql.host`、`mysql.user`、`mysql.password`、`mysql.database` 是否和本地环境一致。
- 是否已经执行 `001_create_short_links.sql`。
- 用户是否有 `SELECT`、`INSERT`、`UPDATE` 权限。

### Redis 开启后跳转变慢或日志报错

检查项：

- Redis 服务是否启动。
- `redis.host`、`redis.port`、`redis.database` 是否正确。
- Redis 只是查询缓存；如果 MySQL 可用，Redis 连接失败不应导致已有短链返回 404。

### 短链重启后丢失

检查项：

- 如果使用 `storage.type=memory`，这是预期行为。
- 需要重启后仍可跳转时，使用 `storage.type=mysql`。

## 构建验证

日常挂载验证命令：

```bash
ssh haoHTTP@orb
cmake -S /Users/hao/Code/haoHTTP -B /tmp/haoHTTP-build
cmake --build /tmp/haoHTTP-build -j2
```

干净克隆验证命令示例：

```bash
ssh haoHTTP@orb
git clone --branch refactor/hao-shortlink-cleanup https://github.com/BeginAgainR/HaoShortLink.git /tmp/haoHTTP-clean-check
cmake -S /tmp/haoHTTP-clean-check -B /tmp/haoHTTP-clean-check-build
cmake --build /tmp/haoHTTP-clean-check-build -j2
```

最近一次 v1.1 干净克隆验证结果：

```text
[100%] Built target shortlink_server
```

最近一次 v1.1 输出文件：

```text
/tmp/haoHTTP-clean-v1.1-final-build/shortlink_server
```

## v1.0 验证记录

v1.0 实现过程中，每个可运行节点都应在 Linux VM 中验证，不在 Mac 宿主机上构建或运行。

### 服务入口和健康检查

状态：已完成。

计划验证内容：

- `cmake -S /Users/hao/Code/haoHTTP -B /tmp/haoHTTP-build`
- `cmake --build /tmp/haoHTTP-build -j2`
- 启动 `/tmp/haoHTTP-build/shortlink_server`
- 使用 curl 请求 `GET /api/health`
- 确认响应状态码为 200，响应体包含 `{"status":"ok"}`

最近一次结果：

```text
[100%] Built target shortlink_server
HTTP/1.1 200 OK
{"status":"ok"}
```

### 创建短链接

状态：已完成。

计划验证内容：

- 使用 curl 请求 `POST /api/short-links`
- 请求体包含 `{"url":"https://example.com/very/long/path"}`
- 确认响应状态码为 201
- 确认响应体包含 `code`、`short_url` 和 `original_url`
- 使用非法 JSON、缺失 `url` 和非法 URL 验证 400 JSON 错误

最近一次结果：

```text
HTTP/1.1 201 Created
{"code":"000001","short_url":"/s/000001","original_url":"https://example.com/very/long/path"}

HTTP/1.1 400 URL must start with http:// or https://
{"error":{"code":"invalid_url","message":"URL must start with http:// or https://"}}

HTTP/1.1 400 Request body must contain a string url field
{"error":{"code":"invalid_request","message":"Request body must contain a string url field"}}
```

### 内存版短链核心

状态：已完成。

最近一次结果：

```text
-- ShortLink Server sources: /Users/hao/Code/haoHTTP/apps/shortlink_server/src/ShortLinkService.cpp
[100%] Built target shortlink_server
```

说明：

- 当前只验证内存版核心参与编译。
- 创建短链接和短码跳转 HTTP 接口已接入。

### 短码跳转

状态：已完成。

计划验证内容：

- 使用创建接口返回的 `short_url` 请求跳转路径
- 确认响应状态码为 302
- 确认 `Location` 指向原始 URL
- 使用不存在的短码验证 404 JSON 错误

最近一次结果：

```text
HTTP/1.1 302 Found
Location: https://example.com/very/long/path

HTTP/1.1 404 Short link not found
{"error":{"code":"short_link_not_found","message":"Short link not found"}}
```

### v1.0 收口验证

状态：已完成。

验证内容：

- 在 Linux VM 中进行最终构建验证。
- 手工跑通健康检查、创建短链接、短码跳转。
- 确认文档只声明 V1 内存版能力。
- 确认 README 和 `docs/` 未声明 MySQL、Redis、Docker Compose、Nginx 等未实现能力。
- 确认仓库内没有生成或提交 `build/` 目录。

日常挂载目录最近一次结果：

```text
[100%] Built target shortlink_server

GET /api/health
HTTP/1.1 200 OK
{"status":"ok"}

POST /api/short-links
HTTP/1.1 201 Created
{"code":"000001","short_url":"/s/000001","original_url":"https://example.com/very/long/path"}

GET /s/000001
HTTP/1.1 302 Found
Location: https://example.com/very/long/path

POST /api/short-links with invalid JSON
HTTP/1.1 400 Request body must contain a string url field
{"error":{"code":"invalid_request","message":"Request body must contain a string url field"}}

POST /api/short-links with invalid URL
HTTP/1.1 400 URL must start with http:// or https://
{"error":{"code":"invalid_url","message":"URL must start with http:// or https://"}}

GET /s/notfound
HTTP/1.1 404 Short link not found
{"error":{"code":"short_link_not_found","message":"Short link not found"}}
```

远端干净克隆验证：

```text
分支：refactor/hao-shortlink-cleanup
提交：d06d7c1
路径：/tmp/haoHTTP-clean-v1.0-remote
构建目录：/tmp/haoHTTP-clean-v1.0-remote-build
结果：[100%] Built target shortlink_server

GET /api/health
HTTP/1.1 200 OK
{"status":"ok"}

POST /api/short-links
HTTP/1.1 201 Created
{"code":"000001","short_url":"/s/000001","original_url":"https://example.com/clean-v1"}

GET /s/000001
HTTP/1.1 302 Found
Location: https://example.com/clean-v1

GET /s/notfound
HTTP/1.1 404 Short link not found
{"error":{"code":"short_link_not_found","message":"Short link not found"}}

POST /api/short-links with invalid URL
HTTP/1.1 400 URL must start with http:// or https://
{"error":{"code":"invalid_url","message":"URL must start with http:// or https://"}}
```

## v1.1 验证计划

当前状态：已完成。

v1.1 已引入 MySQL 持久化和 Redis 查询缓存。MySQL 是事实来源，Redis 只作为短码跳转查询缓存。

### 存储层抽象验证

状态：已完成。

验证内容：

- `ShortLinkService` 依赖 repository 接口。
- 当前默认 repository 仍为内存实现。
- 不改变 V1 对外 API 和响应行为。

最近一次结果：

```text
构建目录：/tmp/haoHTTP-build
结果：[100%] Built target shortlink_server

GET /api/health
HTTP/1.1 200 OK
{"status":"ok"}

POST /api/short-links
HTTP/1.1 201 Created
{"code":"000001","short_url":"/s/000001","original_url":"https://example.com/repository"}

GET /s/000001
HTTP/1.1 302 Found
Location: https://example.com/repository

GET /s/notfound
HTTP/1.1 404 Short link not found
{"error":{"code":"short_link_not_found","message":"Short link not found"}}

POST /api/short-links with invalid URL
HTTP/1.1 400 URL must start with http:// or https://
{"error":{"code":"invalid_url","message":"URL must start with http:// or https://"}}
```

已完成验证项：

- MySQL 表结构脚本可以在 Linux VM 中执行。
- 创建短链接后，MySQL 中存在对应记录。
- 服务重启后，已创建短链接仍可跳转。
- Redis 未命中时可以回源 MySQL。
- Redis 命中时可以直接返回跳转。
- Redis 不可用时不应把已有短链误判为不存在。
- 远端干净克隆验证可以构建并跑通 v1.1 核心接口。

### MySQL 表结构和创建持久化验证

状态：已完成。

已完成内容：

- 新增 `apps/shortlink_server/sql/001_create_short_links.sql`。
- 新增 MySQL repository 创建路径。
- `storage.type=mysql` 时初始化 MySQL 连接池并使用 MySQL repository。
- 创建短链接时写入 MySQL，通过自增 id 生成 Base62 短码并回写 `code`。

当前 VM 已安装：

- `mysql-server`
- `mysql-client`
- MySQL 开发库和 MySQL Connector/C++
- `redis-server`
- `redis-tools`
- `libhiredis-dev`

当前 VM 已创建：

- 数据库：`hao_shortlink`
- 用户：`hao_shortlink`
- 表：`short_links`

最近一次已完成验证：

```text
构建目录：/tmp/haoHTTP-build
结果：[100%] Built target shortlink_server

storage.type=memory 回归：
GET /api/health -> HTTP/1.1 200 OK
POST /api/short-links -> HTTP/1.1 201 Created
GET /s/000001 -> HTTP/1.1 302 Found

storage.type=mysql 验证：
GET /api/health -> HTTP/1.1 200 OK
POST /api/short-links -> HTTP/1.1 201 Created
GET /s/000001 -> HTTP/1.1 302 Found

MySQL 查询结果：
1    000001    https://example.com/mysql-created
```

说明：

- MySQL 创建持久化路径已验证。
- 服务重启后从 MySQL 跳转已验证。

### MySQL 跳转查询验证

状态：已完成。

验证内容：

- `storage.type=mysql` 时，短码跳转从 MySQL 查询原始 URL。
- 服务重启后，进程内状态清空，已创建短链仍可跳转。
- 不存在的短码仍返回 404。

最近一次已完成验证：

```text
构建目录：/tmp/haoHTTP-build
结果：[100%] Built target shortlink_server

GET /api/health -> HTTP/1.1 200 OK
POST /api/short-links -> HTTP/1.1 201 Created
创建短码：000001

MySQL 查询结果：
1    000001    https://example.com/mysql-restart

重启服务后：
GET /s/000001 -> HTTP/1.1 302 Found
Location: https://example.com/mysql-restart

GET /s/notfound -> HTTP/1.1 404 Short link not found
```

### Redis 查询缓存验证

状态：已完成。

已完成内容：

- 新增 Redis 查询缓存组件。
- 新增 Redis 缓存包装 repository。
- `storage.type=mysql` 且 `redis.enabled=true` 时，短码跳转先查 Redis。
- Redis 未命中时回源 MySQL，MySQL 命中后回填 Redis。
- Redis 不可用时继续回源 MySQL，不直接改变为 404。
- `storage.type=memory` 时忽略 Redis 开关。

最近一次已完成验证：

```text
构建目录：/tmp/haoHTTP-build
结果：[100%] Built target shortlink_server

GET /api/health -> HTTP/1.1 200 OK
POST /api/short-links -> HTTP/1.1 201 Created
创建短码：000001

第一次跳转前 Redis：
GET shortlink:000001 -> <nil>

第一次跳转：
GET /s/000001 -> HTTP/1.1 302 Found
Location: https://example.com/redis-cache

第一次跳转后 Redis：
GET shortlink:000001 -> https://example.com/redis-cache

MySQL 临时停止后验证 Redis 命中：
GET /s/000001 -> HTTP/1.1 302 Found
Location: https://example.com/redis-cache

Redis 端口不可用时验证 MySQL 回退：
GET /s/000001 -> HTTP/1.1 302 Found
Location: https://example.com/redis-cache
```

### v1.1 最终收口验证

状态：已完成。

验证内容：

- Linux VM 挂载目录构建。
- MySQL + Redis 模式健康检查。
- 创建短链接后写入 MySQL。
- Redis 首次未命中后回源 MySQL 并回填 Redis。
- 短码跳转返回 302。
- 不存在短码返回 404。
- 非法 URL 返回 400。

最近一次已完成验证：

```text
构建目录：/tmp/haoHTTP-build
结果：[100%] Built target shortlink_server

GET /api/health -> HTTP/1.1 200 OK
POST /api/short-links -> HTTP/1.1 201 Created
创建短码：000001

MySQL 查询结果：
1    000001    https://example.com/v1.1-final

第一次跳转前 Redis：
GET shortlink:000001 -> <nil>

GET /s/000001 -> HTTP/1.1 302 Found
Location: https://example.com/v1.1-final

第一次跳转后 Redis：
GET shortlink:000001 -> https://example.com/v1.1-final

GET /s/notfound -> HTTP/1.1 404 Short link not found
POST /api/short-links with invalid URL -> HTTP/1.1 400 URL must start with http:// or https://
```

远端干净克隆验证：

```text
分支：refactor/hao-shortlink-cleanup
提交：4a53281
路径：/tmp/haoHTTP-clean-v1.1-final
构建目录：/tmp/haoHTTP-clean-v1.1-final-build
结果：[100%] Built target shortlink_server
```

## 当前文档任务验证

文档任务不需要运行构建命令。验证重点是：

- Markdown 文件是否存在。
- README 文档入口是否正确。
- 未实现能力是否明确标注。
- 是否误改 C++ 或 CMake 文件。
