"""
Unified LLM calling logic for SubZeroClaw agent.

Supports both Anthropic Messages API and OpenAI-compatible APIs.
"""

import asyncio
import json
import os
import logging
from pathlib import Path

import httpx

from .tools import build_openai_tools, build_anthropic_tools

log = logging.getLogger("szc.llm")

SZC_HOME = Path(os.environ.get("SZC_HOME", "/agent-data"))
CONFIG_FILE = SZC_HOME / "config.json"


def load_config() -> dict:
    """Load LLM configuration from env vars + config file."""
    defaults = {
        "api_key": os.environ.get("SUBZEROCLAW_API_KEY", ""),
        "model": os.environ.get("SUBZEROCLAW_MODEL", "claude-sonnet-4-20250514"),
        "endpoint": os.environ.get(
            "OPENROUTER_ENDPOINT",
            "https://openrouter.ai/api/v1/chat/completions",
        ),
        "max_turns": 50,
        "max_messages": 40,
    }
    if CONFIG_FILE.exists():
        try:
            saved = json.loads(CONFIG_FILE.read_text())
            defaults.update(saved)
        except Exception:
            pass
    return defaults


def save_config(cfg: dict):
    CONFIG_FILE.write_text(json.dumps(cfg, indent=2))


def is_anthropic(cfg: dict) -> bool:
    return "anthropic.com" in cfg.get("endpoint", "")


def load_system_prompt() -> str:
    """Load the full system prompt from markdown personality/skill files."""
    parts = []
    for subdir in ["core", "skills", "memory"]:
        d = SZC_HOME / subdir
        if not d.is_dir():
            continue
        for f in sorted(d.glob("*.md")):
            parts.append(f.read_text())
    program = SZC_HOME / "autoresearch" / "PROGRAM.md"
    if program.exists():
        parts.append(program.read_text())
    return "\n\n---\n\n".join(parts)


def _anthropic_to_openai(resp_data: dict) -> dict:
    """Normalize Anthropic Messages API response to OpenAI chat completions format."""
    content_blocks = resp_data.get("content", [])
    text_parts = []
    tool_calls = []
    for block in content_blocks:
        if block["type"] == "text":
            text_parts.append(block["text"])
        elif block["type"] == "tool_use":
            tool_calls.append(
                {
                    "id": block["id"],
                    "type": "function",
                    "function": {
                        "name": block["name"],
                        "arguments": json.dumps(block["input"]),
                    },
                }
            )
    msg = {
        "role": "assistant",
        "content": "\n".join(text_parts) if text_parts else "",
    }
    if tool_calls:
        msg["tool_calls"] = tool_calls
    return {
        "choices": [
            {"message": msg, "finish_reason": resp_data.get("stop_reason", "end_turn")}
        ]
    }


def _convert_messages_for_anthropic(
    messages: list[dict],
) -> tuple[str, list[dict]]:
    """Extract system prompt and convert messages to Anthropic format."""
    system = ""
    converted = []
    for m in messages:
        if m["role"] == "system":
            system = m.get("content", "")
        elif m["role"] == "tool":
            converted.append(
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "tool_result",
                            "tool_use_id": m.get("tool_call_id", ""),
                            "content": m.get("content", ""),
                        }
                    ],
                }
            )
        elif m["role"] == "assistant" and m.get("tool_calls"):
            content = []
            if m.get("content"):
                content.append({"type": "text", "text": m["content"]})
            for tc in m["tool_calls"]:
                content.append(
                    {
                        "type": "tool_use",
                        "id": tc["id"],
                        "name": tc["function"]["name"],
                        "input": json.loads(tc["function"]["arguments"]),
                    }
                )
            converted.append({"role": "assistant", "content": content})
        else:
            converted.append({"role": m["role"], "content": m.get("content", "")})
    return system, converted


async def _request_with_retry(client: httpx.AsyncClient, method: str, url: str, **kwargs) -> httpx.Response:
    """Make an HTTP request with retry on 429/529 rate limits."""
    max_retries = 4
    for attempt in range(max_retries + 1):
        resp = await client.request(method, url, **kwargs)
        if resp.status_code in (429, 529) and attempt < max_retries:
            retry_after = float(resp.headers.get("retry-after", 0))
            wait = max(retry_after, 2 ** attempt * 5)  # 5s, 10s, 20s, 40s
            log.warning("Rate limited (%d), retrying in %.0fs (attempt %d/%d)",
                        resp.status_code, wait, attempt + 1, max_retries)
            await asyncio.sleep(wait)
            continue
        if resp.status_code >= 400:
            log.error("LLM API error %d: %s", resp.status_code, resp.text[:500])
        resp.raise_for_status()
        return resp
    if resp.status_code >= 400:
        log.error("LLM API error %d after retries: %s", resp.status_code, resp.text[:500])
    resp.raise_for_status()
    return resp


async def call_llm(messages: list[dict], cfg: dict) -> dict:
    """Call the configured LLM and return OpenAI-format response dict."""
    if is_anthropic(cfg):
        system, anthropic_msgs = _convert_messages_for_anthropic(messages)
        body = {
            "model": cfg["model"],
            "max_tokens": 4096,
            "messages": anthropic_msgs,
            "tools": build_anthropic_tools(SZC_HOME),
        }
        if system:
            body["system"] = system
        async with httpx.AsyncClient(timeout=120) as client:
            resp = await _request_with_retry(
                client, "POST", cfg["endpoint"],
                headers={
                    "x-api-key": cfg["api_key"],
                    "anthropic-version": "2023-06-01",
                    "Content-Type": "application/json",
                },
                json=body,
            )
            return _anthropic_to_openai(resp.json())
    else:
        async with httpx.AsyncClient(timeout=120) as client:
            resp = await _request_with_retry(
                client, "POST", cfg["endpoint"],
                headers={
                    "Authorization": f"Bearer {cfg['api_key']}",
                    "Content-Type": "application/json",
                },
                json={
                    "model": cfg["model"],
                    "messages": messages,
                    "tools": build_openai_tools(SZC_HOME),
                    "tool_choice": "auto",
                    "max_tokens": 4096,
                },
            )
            return resp.json()
