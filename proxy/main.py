"""SubZeroClaw Proxy: credential-injecting reverse proxy for agent MCP requests.

Supports multiple auth modes:
1. Direct: auth_header + auth_value in mcp-servers.json
2. Bearer: auth_value wrapped in "Bearer {value}"
3. Basic: auth_username + auth_password base64 encoded
4. None: no auth injection (for public APIs)
"""

import base64
import json
import os
import time
from pathlib import Path
from typing import Any

import httpx
from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import JSONResponse

from audit import log_request
from rate_limiter import RateLimiter

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SZC_HOME = Path(os.environ.get("SZC_HOME", "/agent-data"))
VAULT_DIR = SZC_HOME / "vault"
MCP_SERVERS_PATH = VAULT_DIR / "mcp-servers.json"

app = FastAPI(title="agent-proxy", version="1.0.0")
limiter = RateLimiter()

_start_time = time.monotonic()

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _load_servers() -> dict[str, Any]:
    """Load and return mcp-servers.json as a dict keyed by server id."""
    try:
        data = json.loads(MCP_SERVERS_PATH.read_text())
    except FileNotFoundError:
        raise HTTPException(status_code=503, detail="MCP server config not found")
    # Support both list and dict formats
    if isinstance(data, list):
        return {s["id"]: s for s in data}
    return data


_HOP_BY_HOP = frozenset(
    {
        "connection",
        "keep-alive",
        "proxy-authenticate",
        "proxy-authorization",
        "te",
        "trailers",
        "transfer-encoding",
        "upgrade",
        "host",
    }
)

_AUTH_HEADERS = frozenset({"authorization", "x-api-key", "proxy-authorization"})


def _forwarded_headers(request: Request) -> dict[str, str]:
    """Copy inbound headers, stripping hop-by-hop and auth headers."""
    out: dict[str, str] = {}
    for k, v in request.headers.items():
        lower = k.lower()
        if lower in _HOP_BY_HOP or lower in _AUTH_HEADERS:
            continue
        out[k] = v
    return out


def _strip_response_auth(headers: httpx.Headers) -> dict[str, str]:
    """Strip auth-related headers from upstream response."""
    out: dict[str, str] = {}
    for k, v in headers.items():
        if k.lower() not in _AUTH_HEADERS and k.lower() not in _HOP_BY_HOP:
            out[k] = v
    return out


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------


@app.get("/health")
async def health():
    try:
        servers = _load_servers()
        enabled = sum(1 for s in servers.values() if s.get("enabled", True))
    except Exception:
        enabled = 0
    uptime = round(time.monotonic() - _start_time, 2)
    return {"status": "ok", "uptime": uptime, "servers_enabled": enabled}


@app.get("/proxy/_catalog")
async def catalog():
    servers = _load_servers()
    items = []
    for sid, srv in servers.items():
        if not srv.get("enabled", True):
            continue
        items.append(
            {
                "id": sid,
                "name": srv.get("name", sid),
                "description": srv.get("description", ""),
                "proxy_path": f"/proxy/{sid}",
            }
        )
    return items


@app.api_route("/proxy/{server_id}/{path:path}", methods=["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"])
async def proxy(server_id: str, path: str, request: Request):
    t0 = time.monotonic()
    source_ip = request.client.host if request.client else "unknown"

    # --- Load server config ---
    servers = _load_servers()
    server = servers.get(server_id)
    if not server:
        raise HTTPException(status_code=404, detail=f"Server {server_id!r} not found")
    if not server.get("enabled", True):
        raise HTTPException(status_code=403, detail=f"Server {server_id!r} is disabled")

    # --- Rate limit ---
    rate = server.get("rate_limit")
    if not limiter.allow(server_id, rate):
        raise HTTPException(status_code=429, detail="Rate limit exceeded")

    # --- Build target URL ---
    base = server["target_base_url"].rstrip("/")
    target_url = f"{base}/{path}"

    # --- Prepare outbound headers ---
    headers = _forwarded_headers(request)

    # --- Inject extra headers (e.g. Notion-Version) ---
    extra_headers = server.get("extra_headers", {})
    for k, v in extra_headers.items():
        headers[k] = v

    # --- Inject auth ---
    auth_type = server.get("auth_type", "none")

    if auth_type == "header":
        # Direct mode: auth_header + auth_value in server config
        header_name = server.get("auth_header", "Authorization")
        auth_value = server.get("auth_value", "")
        if auth_value:
            headers[header_name] = auth_value
    elif auth_type == "bearer":
        auth_value = server.get("auth_value", "")
        if auth_value:
            headers["Authorization"] = f"Bearer {auth_value}"
    elif auth_type == "basic":
        username = server.get("auth_username", "")
        password = server.get("auth_password", "")
        if username or password:
            credentials = base64.b64encode(f"{username}:{password}".encode()).decode()
            headers["Authorization"] = f"Basic {credentials}"
    # auth_type == "none" → nothing to inject

    # --- Forward request ---
    body = await request.body()
    params = dict(request.query_params)

    async with httpx.AsyncClient(timeout=60.0) as client:
        try:
            upstream = await client.request(
                method=request.method,
                url=target_url,
                headers=headers,
                params=params,
                content=body if body else None,
            )
        except httpx.ConnectError:
            latency = (time.monotonic() - t0) * 1000
            log_request(server_id, request.method, path, 502, latency, source_ip)
            raise HTTPException(status_code=502, detail="Failed to connect to upstream")
        except httpx.TimeoutException:
            latency = (time.monotonic() - t0) * 1000
            log_request(server_id, request.method, path, 504, latency, source_ip)
            raise HTTPException(status_code=504, detail="Upstream request timed out")

    # --- Return response ---
    response_headers = _strip_response_auth(upstream.headers)
    latency = (time.monotonic() - t0) * 1000
    log_request(server_id, request.method, path, upstream.status_code, latency, source_ip)

    return Response(
        content=upstream.content,
        status_code=upstream.status_code,
        headers=response_headers,
    )
