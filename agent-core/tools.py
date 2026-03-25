"""
Unified tool definitions and execution for SubZeroClaw agent.

Both dashboard (szc-api) and Telegram bot use these same tools.
"""

import json
import os
import re
import subprocess
import logging
import time
from pathlib import Path

import httpx

log = logging.getLogger("szc.tools")

# Block destructive commands
DANGEROUS_PATTERNS = re.compile(
    r"rm\s+-rf\s+/(?!agent-data/)|mkfs|dd\s+if=|shutdown|reboot|>\s*/dev/sd|"
    r"chmod\s+-R\s+777\s+/|chown\s+-R.*\s+/(?!agent)",
    re.IGNORECASE,
)


def _load_mcp_servers_description(szc_home: Path) -> str:
    """Build a dynamic description of available MCP servers."""
    try:
        mcp_file = szc_home / "vault" / "mcp-servers.json"
        if not mcp_file.exists():
            return "No MCP servers configured."
        data = json.loads(mcp_file.read_text())
        if isinstance(data, list):
            servers = data
        else:
            servers = [{"id": k, **v} for k, v in data.items()]
        enabled = [s for s in servers if s.get("enabled")]
        if not enabled:
            return "No MCP servers are currently enabled."
        lines = ["Available MCP servers (use mcp_request tool to call them):"]
        for s in enabled:
            lines.append(
                f'  - server_id: "{s.get("id", "unknown")}" | {s.get("name", "?")}:'
                f' {s.get("description", "N/A")} | base: {s.get("target_base_url", "N/A")}'
            )
        return "\n".join(lines)
    except Exception:
        return "MCP servers unavailable."


def _mcp_tool_params():
    return {
        "type": "object",
        "properties": {
            "server_id": {"type": "string", "description": "The MCP server ID to call"},
            "method": {
                "type": "string",
                "enum": ["GET", "POST", "PUT", "DELETE", "PATCH"],
                "description": "HTTP method",
            },
            "path": {
                "type": "string",
                "description": "Path to append to the server's base URL (e.g. '/search?q=hello')",
            },
            "body": {
                "type": "string",
                "description": "JSON request body (for POST/PUT/PATCH)",
            },
        },
        "required": ["server_id", "method", "path"],
    }


def build_openai_tools(szc_home: Path) -> list[dict]:
    """Build OpenAI-format tool definitions."""
    tools = [
        {
            "type": "function",
            "function": {
                "name": "shell",
                "description": "Execute a shell command on the host system",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "command": {"type": "string", "description": "The shell command to execute"}
                    },
                    "required": ["command"],
                },
            },
        }
    ]
    mcp_desc = _load_mcp_servers_description(szc_home)
    if "server_id" in mcp_desc:
        tools.append(
            {
                "type": "function",
                "function": {
                    "name": "mcp_request",
                    "description": (
                        "Make an HTTP request to an MCP server through the credential proxy. "
                        f"Auth is injected automatically.\n{mcp_desc}"
                    ),
                    "parameters": _mcp_tool_params(),
                },
            }
        )
    return tools


def build_anthropic_tools(szc_home: Path) -> list[dict]:
    """Build Anthropic-format tool definitions."""
    tools = [
        {
            "name": "shell",
            "description": "Execute a shell command on the host system",
            "input_schema": {
                "type": "object",
                "properties": {
                    "command": {"type": "string", "description": "The shell command to execute"}
                },
                "required": ["command"],
            },
        }
    ]
    mcp_desc = _load_mcp_servers_description(szc_home)
    if "server_id" in mcp_desc:
        tools.append(
            {
                "name": "mcp_request",
                "description": (
                    "Make an HTTP request to an MCP server through the credential proxy. "
                    f"Auth is injected automatically.\n{mcp_desc}"
                ),
                "input_schema": _mcp_tool_params(),
            }
        )
    return tools


def run_shell(command: str, timeout: int = 60) -> str:
    """Execute a shell command and return combined stdout+stderr."""
    if DANGEROUS_PATTERNS.search(command):
        return "BLOCKED: potentially destructive command detected"
    log.info("SHELL: %s", command[:200])
    try:
        result = subprocess.run(
            command, shell=True, capture_output=True, text=True, timeout=timeout
        )
        output = (result.stdout + result.stderr).strip()
        if len(output) > 16000:
            output = output[:8000] + "\n...[truncated]...\n" + output[-8000:]
        return output or "(no output)"
    except subprocess.TimeoutExpired:
        return f"ERROR: timed out after {timeout}s"
    except Exception as e:
        return f"ERROR: {e}"


async def run_mcp_request(
    server_id: str, method: str, path: str, body: str = None
) -> str:
    """Route a request through the credential proxy for auth injection."""
    proxy_url = os.environ.get("SZC_PROXY_URL", "http://agent-proxy:9090")
    url = f"{proxy_url}/proxy/{server_id}/{path.lstrip('/')}"
    headers = {"Accept-Encoding": "identity"}
    if body:
        headers["Content-Type"] = "application/json"
    try:
        async with httpx.AsyncClient(timeout=30, follow_redirects=True) as client:
            resp = await client.request(
                method=method,
                url=url,
                headers=headers,
                content=body.encode() if body else None,
            )
            text = resp.text
            if len(text) > 16000:
                text = text[:8000] + "\n...[truncated]...\n" + text[-8000:]
            return f"HTTP {resp.status_code}\n{text}"
    except httpx.TimeoutException:
        return "ERROR: MCP request timed out"
    except Exception as e:
        return f"ERROR: MCP request failed: {e}"


async def execute_tool(tool_name: str, args: dict) -> str:
    """Execute a tool call and return the output string."""
    if tool_name == "mcp_request":
        return await run_mcp_request(
            server_id=args.get("server_id", ""),
            method=args.get("method", "GET"),
            path=args.get("path", ""),
            body=args.get("body"),
        )
    elif tool_name == "shell":
        return run_shell(args.get("command", ""))
    else:
        return f"ERROR: Unknown tool: {tool_name}"
