# 测试计划

状态：持续维护；当前基线为 v1.4 全量回归通过
当前实现：已建立框架与业务基础测试、API 冒烟、MySQL / Redis 集成、Redis 不可用回退、异常场景和 Compose 编排入口；增强后的 CI 已通过构建、CTest、API smoke、脚本语法和依赖集成验证，v1.4.7 全量回归已完成。

## v1.3 执行顺序

v1.3 的目标是把当前手工验证沉淀为可重复执行的测试体系，已完成第一版收口。

1. 文档状态对齐：
   - 对齐 README、路线图、决策记录和测试计划中的阶段状态。
   - 确认已完成能力和未实现能力没有混写。
2. 最小测试骨架：
   - 状态：已完成。
   - 已新增测试目录和最小测试可执行程序。
   - 已接入 CMake 测试构建入口。
   - 测试程序失败时会返回非 0。
3. 框架基础测试：
   - 状态：已完成第一批基础测试。
   - 覆盖路由匹配、路径参数、请求解析、响应构造和中间件顺序。
   - 优先覆盖曾经修复过的框架问题。
4. 短链业务纯逻辑测试：
   - 状态：已完成第一批基础测试。
   - 覆盖 URL 校验、内存 repository 创建和查询、短码不存在等场景。
   - 不依赖 MySQL、Redis 或 Docker Compose。
5. API 冒烟测试：
   - 状态：已完成第一批脚本。
   - 将健康检查、创建短链、短码跳转和基础错误响应沉淀为脚本。
   - 优先使用内存存储模式，降低依赖复杂度。
6. MySQL / Redis 集成测试：
   - 状态：已完成第一批依赖已启动版脚本。
   - 已完成 Redis 不可用回退 MySQL 脚本。
   - 已完成 Compose 依赖编排脚本。
   - 覆盖 MySQL 持久化、Redis 未命中回源、Redis 回填和 Redis 不可用回退。
   - 依赖 Docker Compose，当前 CI 增强已加入该回归入口。
7. CI：
   - 状态：已完成第一版 workflow，核心命令链路已在 Linux VM 中验证，GitHub Actions 云端 CI 已通过。
   - 第一版覆盖 Linux 构建、CTest 和 API 冒烟测试。
   - 当前增强增加 Bash 语法检查、MySQL / Redis Compose 集成和 Redis 不可用 fallback。

## CI 当前方案

当前 CI 在第一版 Linux 构建、CTest 和 API smoke 基础上，增加脚本语法与 MySQL / Redis 依赖集成验证。

当前 workflow：`.github/workflows/ci.yml`。

触发范围：

- 推送到主线分支、`refactor/**` 或 `feature/**` 开发分支时执行。
- 发起 pull request 时执行。

当前覆盖：

- 安装 C++ 构建依赖和运行测试所需工具。
- 准备 `muduo_base` 和 `muduo_net`。
- 执行 CMake 配置和构建。
- 执行 CTest，覆盖框架基础测试和短链业务纯逻辑测试。
- 执行 API 冒烟测试，使用内存存储模式验证健康检查、创建短链、短码跳转和基础错误响应。
- 检查 `tests/scripts/*.sh` 的 Bash 语法。
- 通过 Docker Compose 启动 MySQL、Redis，执行持久化、缓存回填和 Redis 不可用 fallback 测试。
- muduo 构建命令兼容新版 CMake 对旧项目最低版本策略的检查。

当前暂不覆盖：

- Nginx 反向代理验证。
- 压测、限流和可观测性验证。

依赖集成 CI 边界：

- GitHub runner 使用 `127.0.0.1` 访问 Compose 发布端口，不使用 OrbStack 的 `docker.orb.internal` 或 IPv6 规避地址。
- 失败时输出 MySQL / Redis 容器日志，结束时清理容器和临时数据卷。
- 当前不把环境敏感的性能基线作为 PR 硬门禁。

最近一次增强 CI 验证：

- commit：`6170d40`。
- GitHub Actions run：`29138145564`。
- 结果：checkout v7、Shell 语法、Linux 构建、CTest、API smoke、MySQL / Redis 集成和 Redis 不可用 fallback 全部通过。

## 当前测试入口

当前测试入口基于 CMake / CTest。构建和运行测试应在 Linux VM 或容器环境中执行，不在 Mac
宿主机上执行。

```bash
cmake -S . -B /tmp/haoHTTP-build
cmake --build /tmp/haoHTTP-build
ctest --test-dir /tmp/haoHTTP-build --output-on-failure
bash tests/scripts/api_smoke_test.sh
HAOHTTP_TEST_HOST=haoHTTP@orb bash tests/scripts/run_integration_with_compose.sh
```

## 测试分层

### 构建验证

目标：

- 确认项目在 Linux 环境中可构建。
- 防止基础编译错误进入后续任务。

当前状态：已通过 Linux VM 构建验证、v1.1 干净克隆验证、v1.2 本地干净克隆 Compose 验证、
v1.3 最小测试骨架验证、第一批框架基础测试验证、短链业务纯逻辑测试验证、API 冒烟测试验证、
MySQL / Redis 集成测试验证、Redis 不可用回退测试验证、Compose 依赖编排验证、CI 第一版核心命令链路验证、GitHub Actions 云端 CI 验证和 v1.4.7 全量回归验证。

最近一次验证：

- 环境：OrbStack Linux VM `haoHTTP`
- 类型：v1.4.7 全量回归验证
- 分支：`refactor/v1.3-tests-ci`
- 项目路径：`/Users/hao/Code/haoHTTP`
- 构建目录：`/tmp/haoHTTP-build`
- 命令：Linux VM 中执行构建、CTest、`api_smoke_test.sh` 和 `shortlink_exception_scenarios_test.sh`；Mac 侧通过 `HAOHTTP_TEST_HOST=haoHTTP@orb bash tests/scripts/run_integration_with_compose.sh` 编排依赖并执行集成与 fallback 测试。
- 结果：Linux VM 中 `shortlink_server` 构建通过，CTest `1/1`、API smoke、MySQL / Redis 集成、Redis 不可用 fallback 和异常场景脚本均通过；独立干净克隆目录为 `/tmp/haoHTTP-v1.4-clean.fjetXo`，异常场景 artifact 为 `/tmp/haohttp-exception-scenarios.EUZdzv`

补充验证：

- 已验证 workflow 中 muduo tarball 下载、配置和 `muduo_base` / `muduo_net` 构建命令。
- 验证过程中发现新版 CMake 会拒绝 muduo 旧版本过低的 `cmake_minimum_required`，已在 workflow 和 Dockerfile 的 muduo 构建命令中补充 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`。

触发条件：

- 修改 C++ 源码或头文件后，应在 Linux VM 中执行构建验证。
- 修改 CMake、依赖或构建相关配置后，应执行干净克隆验证。
- 每个版本节点完成前，至少执行一次干净克隆验证。

### 框架测试

目标：

- 验证路由、请求解析、响应构造、中间件等框架能力。

当前状态：已建立第一批基础测试，覆盖路由、请求、响应和中间件顺序。

### API 测试

目标：

- 验证健康检查、创建短链接和短码跳转接口。

当前状态：健康检查、创建短链接和短码跳转已通过手工 curl 验证。
已新增第一批 API 冒烟测试脚本，覆盖健康检查、创建短链、短码跳转、短码不存在和非法 URL。

### 集成测试

目标：

- 验证服务、数据库、缓存等组件协作。

当前状态：MySQL 持久化、Redis 查询缓存和 Nginx 反向代理已通过手工集成验证；已新增并验证第一批
MySQL / Redis 自动化集成测试脚本、Redis 不可用回退测试脚本和 Compose 依赖编排脚本。

### 压测

目标：

- 验证吞吐、延迟和稳定性。

当前状态：v1.4 已完成 curl fallback 和 `hey` 多模式轻量基线；不声明生产承载能力。

## 当前阶段检查项

- 文档是否准确描述当前项目状态。
- 未实现功能是否没有写成已完成。
- 已知问题是否记录在 `BUGS.md`。

## Review 检查方法

每次 review 优先检查：

- 变更范围是否符合任务目标。
- diff 是否包含无关文件。
- 是否违反当前阶段约束。
- 是否把未实现能力写成已完成。
- 是否需要在 Linux VM 中补充构建或测试验证。
