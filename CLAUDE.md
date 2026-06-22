# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Git

Do not add Co-Authored-By lines to commit messages.

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

**Dependencies** (must be installed): `muduo`, `OpenSSL`, `mysqlcppconn` / `mysqlclient`.

Run the server (default port 80, requires root or capability):
```bash
./simple_server -p 8080
```

There are no automated tests; manual testing is done via `HttpServer/examples/test_client.cc`.

## Architecture

The project has two layers:

- **`HttpServer/`** — reusable HTTP framework library
- **`WebApps/GomokuServer/`** — an example web app (online Gomoku game) built on top of it

### Framework request lifecycle

```
TcpServer (Muduo)
  └─ onMessage()        # raw bytes → HttpContext parses into HttpRequest
       └─ onRequest()   # MiddlewareChain::before() → Router::route() → MiddlewareChain::after()
            └─ Router   # exact match (unordered_map) or regex match (vector<regex>)
                 └─ RouterHandler::handle() or HandlerCallback lambda
```

### Key design decisions

**Router** (`HttpServer/include/router/Router.h`): Two matching strategies on the same `Router` object — a hash map for exact paths and a `std::vector<regex>` for dynamic paths like `/users/:id`. Two registration styles exist in parallel: subclass `RouterHandler` for complex handlers with multiple methods, or pass a lambda/`std::function` for simple ones.

**Middleware** (`HttpServer/include/middleware/Middleware.h`): Pure virtual `before(request)` / `after(response)` interface. Middleware objects are chained via `MiddlewareChain` and run around every request. `CorsMiddleware` is the only built-in implementation.

**HttpServer** (`HttpServer/include/http/HttpServer.h`): Owns a Muduo `TcpServer`, a `Router`, a `SessionManager`, a `MiddlewareChain`, and (optionally) an `SslContext`. SSL is handled by wrapping each `TcpConnectionPtr` in an `SslConnection`, stored in `sslConns_` map.

**Session** (`HttpServer/include/session/`): `SessionManager` creates/looks up `Session` objects; `SessionStorage` is the persistence interface (in-memory or DB-backed).

**DB pool** (`HttpServer/include/utils/db/`): `DbConnectionPool` manages MySQL connections. `DbConnection` wraps a single connection. `DbException` is thrown on errors.

### Adding a new route to GomokuServer

1. Create a handler header in `WebApps/GomokuServer/include/handlers/` extending `http::router::RouterHandler`.
2. Implement `handle(const HttpRequest&, HttpResponse*)` in `WebApps/GomokuServer/src/handlers/`.
3. Register in `GomokuServer.cpp` via `server.Get("/path", std::make_shared<MyHandler>())` or `server.Post(...)`.

### Namespaces

- Framework code lives in `namespace http` with sub-namespaces `http::router`, `http::middleware`, `http::session`, `http::ssl`.
- `GomokuServer` app code does not use a namespace.
