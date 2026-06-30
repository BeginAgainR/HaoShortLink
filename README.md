# HaoShortLink

HaoShortLink is a C++17 HTTP framework project based on the muduo network
library. The repository is being refactored from the old demo application into
a future short link service.

## Current Status

- `HttpServer/` contains the existing reusable HTTP framework.
- `apps/shortlink_server/` is the placeholder application directory for the
  future short link server.
- Short link business logic and APIs are not implemented yet.
- Build and runtime verification will be handled later in a Linux VM or
  container environment.

## Project Layout

```text
HttpServer/                 Existing HTTP framework
apps/shortlink_server/      Placeholder short link server application
  include/
  src/
  config/
  sql/
```

## Notes

Keep framework changes small and isolated. Do not add short link dependencies or
business behavior until that work is explicitly scoped.
