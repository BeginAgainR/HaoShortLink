# API 设计

状态：草案
当前实现：已实现短链接核心接口，支持内存存储、MySQL 持久化和可选 Redis 查询缓存

## 路径命名原则

- 面向程序调用的接口放在 `/api/` 下。
- 面向用户直接访问和分享的短链跳转路径保持简短。
- 使用名词表示资源，使用 HTTP 方法表示动作。
- 不提前设计 V1 不需要的接口。

## V1 接口

V1 接口已实现。默认存储为进程内存；v1.1 增加 MySQL 持久化和可选 Redis 查询缓存，不改变对外 API。

### 健康检查

当前状态：已实现。

```text
GET /api/health
```

用途：

确认服务是否可访问。

响应示例：

状态码：

```text
200 OK
```

响应体：

```json
{
  "status": "ok"
}
```

### 创建短链接

当前状态：已实现。

```text
POST /api/short-links
```

用途：

提交原始 URL，创建一个短链接。

请求头：

```text
Content-Type: application/json
```

请求示例：

```json
{
  "url": "https://example.com/very/long/path"
}
```

V1 URL 校验规则：

- `url` 字段必须存在。
- `url` 字段必须是字符串。
- `url` 字段必须以 `http://` 或 `https://` 开头。
- 不在 V1 中实现域名解析、黑名单、过期时间或复杂安全策略。

成功状态码：

```text
201 Created
```

响应示例：

```json
{
  "code": "000001",
  "short_url": "/s/000001",
  "original_url": "https://example.com/very/long/path"
}
```

错误场景：

- 请求体不是合法 JSON：`400 Bad Request`。
- 缺少 `url` 字段：`400 Bad Request`。
- `url` 不是字符串或不符合 V1 最小 URL 规则：`400 Bad Request`。

### 短码跳转

当前状态：已实现。

```text
GET /s/{code}
```

用途：

根据短码查找原始 URL，并返回重定向响应。

成功状态码：

```text
302 Found
```

行为：

```text
302 Found
Location: https://example.com/very/long/path
```

错误场景：

- 短码不存在：`404 Not Found`。

## 错误响应

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

V1 业务错误示例：

```json
{
  "error": {
    "code": "invalid_request",
    "message": "Invalid request"
  }
}
```

```json
{
  "error": {
    "code": "short_link_not_found",
    "message": "Short link not found"
  }
}
```

## 待补充

- API 是否引入版本号，例如 `/api/v1`。
