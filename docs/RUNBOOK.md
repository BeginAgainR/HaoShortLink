# 运行手册

状态：草案
当前实现：当前阶段不在 Mac 宿主机上构建或运行。

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
- 开发依赖：`libssl-dev`、`libmysqlclient-dev`、`libmysqlcppconn-dev`
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
```

说明：

- 空行和以 `#` 开头的整行注释会被忽略。
- key 和 value 两侧空白会被裁剪。
- 当前示例仅用于后续服务入口接入配置加载，不代表服务启动命令已经完成。

待补充内容：

- 服务启动命令。
- 常见故障排查。

## 构建验证

当前构建命令：

```bash
ssh haoHTTP@orb
cmake -S /Users/hao/Code/haoHTTP -B /tmp/haoHTTP-build
cmake --build /tmp/haoHTTP-build -j2
```

当前结果：

```text
[100%] Built target shortlink_server
```

输出文件：

```text
/tmp/haoHTTP-build/shortlink_server
```

## 当前文档任务验证

文档任务不需要运行构建命令。验证重点是：

- Markdown 文件是否存在。
- README 文档入口是否正确。
- 未实现能力是否明确标注。
- 是否误改 C++ 或 CMake 文件。
