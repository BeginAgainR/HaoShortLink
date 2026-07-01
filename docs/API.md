# API 设计

状态：草案
当前实现：尚未实现

## 路径命名原则

- 面向程序调用的接口放在 `/api/` 下。
- 面向用户直接访问和分享的短链跳转路径保持简短。
- 使用名词表示资源，使用 HTTP 方法表示动作。
- 不提前设计 V1 不需要的接口。

## 暂定接口

### 健康检查

```text
GET /api/health
```

用途：

确认服务是否可访问。

响应示例草案：

```json
{
  "status": "ok"
}
```

### 创建短链接

```text
POST /api/short-links
```

用途：

提交原始 URL，创建一个短链接。

请求示例草案：

```json
{
  "url": "https://example.com/very/long/path"
}
```

响应示例草案：

```json
{
  "code": "abc123",
  "short_url": "/s/abc123",
  "original_url": "https://example.com/very/long/path"
}
```

### 短码跳转

```text
GET /s/{code}
```

用途：

根据短码查找原始 URL，并返回重定向响应。

暂定行为：

```text
302 Found
Location: https://example.com/very/long/path
```

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

## 待补充

- URL 校验规则。
- 短码字符集和长度。
- 请求和响应字段的完整规范。
- API 是否引入版本号，例如 `/api/v1`。
