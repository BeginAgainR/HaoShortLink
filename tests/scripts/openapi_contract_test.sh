#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

command -v python3 >/dev/null 2>&1 || {
    echo "FAIL: python3 is required" >&2
    exit 1
}
if command -v ruby >/dev/null 2>&1; then
    ruby -ryaml -e '
document = YAML.safe_load(File.read(ARGV.fetch(0)), aliases: true)
abort "FAIL: OpenAPI YAML root must be a mapping" unless document.is_a?(Hash)
abort "FAIL: OpenAPI YAML must declare 3.1.0" unless document["openapi"] == "3.1.0"
abort "FAIL: OpenAPI YAML paths must be a mapping" unless document["paths"].is_a?(Hash)
' "${ROOT_DIR}/docs/openapi.yaml"
fi

python3 - "${ROOT_DIR}" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
openapi_path = root / "docs/openapi.yaml"
source_paths = [
    root / "apps/shortlink_server/src/AuthHttp.cpp",
    root / "apps/shortlink_server/src/ShortLinkHttpApi.cpp",
]

document = openapi_path.read_text(encoding="utf-8")
if not document.startswith("openapi: 3.1.0\n"):
    raise SystemExit("FAIL: OpenAPI document must declare version 3.1.0")
if "      name: hao_session\n" not in document:
    raise SystemExit("FAIL: OpenAPI cookie security scheme is missing hao_session")

document_routes = set()
current_path = None
for line in document.splitlines():
    path_match = re.fullmatch(r"  (/[^:]+):", line)
    if path_match:
        current_path = path_match.group(1)
        continue
    method_match = re.fullmatch(r"    (get|post|put|delete|patch):", line)
    if current_path and method_match:
        document_routes.add((method_match.group(1).upper(), current_path))

source = "\n".join(path.read_text(encoding="utf-8") for path in source_paths)
source_routes = set()
for method, path in re.findall(r'server->(Get|Post)\(\s*"([^"]+)"', source):
    source_routes.add((method.upper(), path))
for method, path in re.findall(
    r'server->addRoute\(\s*http::HttpRequest::k(Get|Post|Put|Delete),\s*"([^"]+)"',
    source,
    re.DOTALL,
):
    source_routes.add((method.upper(), path))

source_routes.discard(("GET", "/metrics"))
source_routes = {
    (method, re.sub(r":([A-Za-z_][A-Za-z0-9_]*)", r"{\1}", path))
    for method, path in source_routes
}

missing = sorted(source_routes - document_routes)
extra = sorted(document_routes - source_routes)
if missing or extra:
    details = []
    if missing:
        details.append("missing from OpenAPI: " + ", ".join(f"{m} {p}" for m, p in missing))
    if extra:
        details.append("not registered by the server: " + ", ".join(f"{m} {p}" for m, p in extra))
    raise SystemExit("FAIL: OpenAPI route drift; " + "; ".join(details))

if any(path.startswith("/internal/") for _, path in document_routes):
    raise SystemExit("FAIL: legacy internal routes must not be part of the v2.0 OpenAPI")

print(f"PASS: OpenAPI matches all {len(document_routes)} public server routes")
PY
