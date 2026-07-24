# v2.0 验收记录

状态：v2.0.5 本地验收通过；远端交付状态以对应 Pull Request 检查记录为准
验收日期：2026-07-21
验证环境：OrbStack Linux VM `haoHTTP`、OrbStack Docker、MySQL 8.0、Redis 7、Kafka 4.3.1

## 范围结论

v2.0 已形成普通用户产品闭环：用户可以注册、登录、创建随机或自定义短码，只管理自己的链接，并从
`/app/` 完成生命周期和访问统计操作；公共访问者无需登录即可跳转。

本版本不实现平台管理员、跨用户管理或 RBAC。“管理”是用户对自己链接的对象级管理。历史链接归属禁用的
`legacy-system`。v2.1 Kubernetes 和 v2.2 Transactional Outbox 仍是后续阶段，未混入本次完成声明。

## 需求证据矩阵

| 要求 | 权威证据 | 结果 |
| --- | --- | --- |
| 注册、登录、退出、当前用户 | `AuthHttp` / `AuthService`、CTest、API smoke、MySQL 集成 | 通过 |
| 密码安全 | OpenSSL scrypt、随机 salt、常量时间比较、数据库格式与明文排除断言 | 通过 |
| 持久化会话 | 32 字节随机 token、数据库 SHA-256 摘要、固定过期、撤销、跨进程重启测试 | 通过 |
| 账号禁用 | 认证查询联表检查 active；禁用后旧会话与登录统一失败测试 | 通过 |
| 浏览器同源边界 | `SameOriginPolicy` 单元测试与跨站登录 / 创建 API 测试 | 通过 |
| owner 与水平越权 | owner-aware repository / SQL；两个用户列表、详情、更新和统计 404 测试 | 通过 |
| 随机 / 自定义短码 | 格式、保留字、大小写、跨用户全局唯一和并发冲突测试 | 通过 |
| 生命周期 | owner 范围详情 / 分页 / 筛选、禁用 / 恢复、过去 / 未来 / 空过期测试 | 通过 |
| 匿名跳转 | 无 Cookie 的 302，以及不存在 / 禁用 / 过期统一 404 | 通过 |
| 管理页面 | 实际浏览器注册、创建、筛选、生命周期、统计、冲突、退出 / 登录和跳转 | 通过 |
| OpenAPI | YAML 解析、Cookie scheme、从 C++ 注册点提取的 13 个公开 method/path 精确集合比较 | 通过 |
| schema migration | 隔离 MySQL 空库、v1.9 升级、owner 回填、幂等、回滚、回滚后再升级 | 通过 |
| Redis / 健康 / 限流 | cache v2、fallback、Lua 并发额度、fail-open、MySQL readiness 恢复 | 通过 |
| 页面与监控入口 | Compose、Nginx、静态资源、Prometheus、Grafana、持久化重建 smoke | 通过 |
| Kafka 与统计 | producer / consumer、owner 统计授权、幂等、DLQ、lag、broker / MySQL 恢复、重放重建 | 通过 |
| 应用层收口 | `main.cpp` 只组装依赖；配置、认证和 HTTP API 拆分；`HttpServer/` 无改动 | 通过 |
| 独立目录 | 无 `.git`、无既有构建产物的复制目录重新配置、编译、CTest、契约和 API | 通过 |

## 执行记录

### Linux 构建、单元与 API

工作区构建目录：`/tmp/haoHTTP-build`

```bash
cmake -S . -B /tmp/haoHTTP-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/haoHTTP-build -j2
ctest --test-dir /tmp/haoHTTP-build --output-on-failure
HAOHTTP_BUILD_DIR=/tmp/haoHTTP-build bash tests/scripts/api_smoke_test.sh
```

结果：

- `shortlink_server` 与 `shortlink_event_consumer` Release 构建成功。
- CTest 通过；测试可执行程序报告 `35 test(s) passed`。
- API smoke 全部通过，覆盖认证、Cookie 属性、同源拒绝、自定义短码、分页、生命周期、跳转、敏感 token
  不进入错误 / 日志 / 指标、退出、重新登录和关闭注册。

### OpenAPI 与脚本静态检查

```bash
bash tests/scripts/openapi_contract_test.sh
for script in tests/scripts/*.sh; do bash -n "${script}"; done
docker compose config
docker compose -f compose.yaml -f compose.kafka.yaml config
git diff --check
```

结果：OpenAPI YAML 可解析，13 个公开路由与服务注册点一致；Shell、Compose 和 diff 检查通过。

### 数据迁移

```bash
SHORTLINK_MYSQL_IMAGE=mysql:8.0 bash tests/scripts/schema_migration_test.sh
```

隔离 MySQL 结果：

- 空数据库顺序建立 001-005。
- v1.9 历史链接的 URL、`disabled` 状态、过期时间和统计值保持不变。
- 历史链接 owner 回填到禁用的 `legacy-system`。
- 再次执行 migration 不产生第二个系统用户或重复对象。
- 显式 down 删除 v2.0 用户 / 会话 / owner，但保留 v1.9 链接与统计。
- down 后再次 up 可以恢复 v2.0 schema 和 owner 回填。

### MySQL、Redis、健康和异常

```bash
HAOHTTP_TEST_HOST=haoHTTP@orb \
HAOHTTP_TEST_WORKDIR=/Users/hao/Code/haoHTTP \
HAOHTTP_BUILD_DIR=/tmp/haoHTTP-build \
bash tests/scripts/run_integration_with_compose.sh

HAOHTTP_BUILD_DIR=/tmp/haoHTTP-build \
HAOHTTP_MYSQL_HOST=docker.orb.internal \
HAOHTTP_REDIS_HOST=docker.orb.internal \
bash tests/scripts/shortlink_exception_scenarios_test.sh
```

结果：MySQL / Redis integration、Redis unavailable fallback、限流、MySQL readiness 故障恢复和异常矩阵
全部通过。集成测试直接确认 scrypt 不含明文、原始会话 token 未落库、摘要存在、并发唯一性、owner 隔离、
会话跨服务重启、绝对过期和账号禁用。

### Compose 页面、Nginx 和监控

```bash
bash tests/scripts/monitoring_smoke_test.sh
bash tests/scripts/rate_limit_nginx_test.sh
```

结果：管理页面、OpenAPI、认证创建和跳转可用；Prometheus target / 查询、Grafana datasource / dashboard、
监控数据卷重建保持通过；Nginx 不暴露 `/metrics`，旧 `/internal/` 在 Nginx 和应用直连都为 `404`，
未认证管理请求为 `401`。

### 实际浏览器闭环

使用同一 Compose 入口 `http://127.0.0.1:8080/app/` 完成：

1. 注册并进入工作台。
2. 创建带过期时间的自定义短码。
3. 列表、状态筛选和空状态反馈。
4. 禁用、恢复和内联修改过期时间。
5. 查看空统计及最终一致性提示。
6. 重复短码冲突反馈。
7. 退出，以不同大小写用户名重新登录。
8. 匿名访问短码并最终到达目标 URL。

测试用户、链接和缓存夹具随后已清理。当前未引入 Node.js / Playwright CI；该实际浏览器证据属于 v2.0
本地版本验收。

### Kafka、统计与恢复

```bash
HAOHTTP_TEST_HOST=haoHTTP@orb \
HAOHTTP_TEST_WORKDIR=/Users/hao/Code/haoHTTP \
HAOHTTP_BUILD_DIR=/tmp/haoHTTP-build \
SHORTLINK_KAFKA_EXTERNAL_HOST=docker.orb.internal \
HAOHTTP_KAFKA_BOOTSTRAP_SERVERS=docker.orb.internal:19092 \
HAOHTTP_MYSQL_HOST=docker.orb.internal \
bash tests/scripts/run_kafka_integration_with_compose.sh
```

结果：事件契约、producer 指标、统计事务、重复、ignored、owner 统计 404、consumer 观测、DLQ、手动 offset、
重启、有界关闭、producer 初始化失败、Kafka fail-open、broker 恢复、MySQL 恢复、DLQ 恢复、幂等重放、
隔离数据库重建和 Kafka UI 全部通过。

### 无历史产物的独立目录

源码目录：`/tmp/haoHTTP-v20-clean.91g5Fo`
构建目录：`/tmp/haoHTTP-v20-build.h7CGzN`

源码由当前工作区复制但排除 `.git`、`build/` 和宿主元数据；验证确认 `.git` 不存在。随后重新执行 Release
配置 / 编译、CTest、全部 Shell 语法、OpenAPI 路由契约和 API smoke，结果全部通过。该验证证明新头文件、
源码、页面、SQL 和脚本没有依赖工作区中未复制的隐藏构建产物。

## 已知边界

- GitHub Actions workflow 已加入 OpenAPI 与 schema migration 门禁；发布交付时由对应 Pull Request 的
  远端检查再次验证。
- HTTPS/TLS 终止尚未实现；真实 HTTPS 部署必须设置 `auth.cookie_secure=true`。
- 过期和已撤销会话按读取时间失效，但当前没有自动清理旧 `user_sessions` 行。
- 管理页面浏览器验收当前是版本节点实际操作，不是 CI 浏览器自动化。
- Kafka producer 仍为 fail-open，topic retention 为 7 天；统计不承诺 producer 到 MySQL 端到端 exactly-once
  或 retention 外重建。
- 本文件记录本地验收证据，不替代提交历史、Pull Request 检查或发布记录。

## 验收结论

v2.0 产品、权限、安全、迁移、页面、OpenAPI、旧能力回归和应用层收口的本地完成定义已经满足。
后续开发应进入 v2.1 Kubernetes 范围，不应在 v2.0 回补平台管理员或提前混入 v2.2 Outbox。
