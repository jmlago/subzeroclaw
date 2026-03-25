"""
SubZeroClaw Daily Review — Autonomous Self-Assessment

Runs once daily via cron. Uses the LLM to:
1. Review recent heartbeat logs for patterns
2. Check project/pipeline status
3. Suggest next actions
4. Send a brief daily summary to Telegram
"""

import os
import json
import subprocess
import logging
from datetime import datetime, timezone
from pathlib import Path

import httpx

logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s: %(message)s")
log = logging.getLogger("agent-daily")

TELEGRAM_TOKEN = os.environ["TELEGRAM_BOT_TOKEN"]
OWNER_CHAT_ID = os.environ.get("TELEGRAM_ALLOWED_USERS", "").split(",")[0]
SZC_HOME = Path(os.environ.get("SZC_HOME", "/agent-data"))


def shell(cmd: str, timeout: int = 30) -> str:
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return (r.stdout + r.stderr).strip()[:4000]
    except Exception as e:
        return f"ERROR: {e}"


def load_config() -> dict:
    cfg_file = SZC_HOME / "config.json"
    defaults = {
        "api_key": os.environ.get("SUBZEROCLAW_API_KEY", ""),
        "model": os.environ.get("SUBZEROCLAW_MODEL", ""),
        "endpoint": os.environ.get("OPENROUTER_ENDPOINT", ""),
    }
    if cfg_file.exists():
        try:
            defaults.update(json.loads(cfg_file.read_text()))
        except Exception:
            pass
    return defaults


def call_anthropic(cfg: dict, system: str, user_msg: str) -> str:
    """Call Anthropic API directly (no tools needed for review)."""
    if "anthropic.com" in cfg.get("endpoint", ""):
        resp = httpx.post(
            cfg["endpoint"],
            headers={
                "x-api-key": cfg["api_key"],
                "anthropic-version": "2023-06-01",
                "Content-Type": "application/json",
            },
            json={
                "model": cfg["model"],
                "max_tokens": 1500,
                "system": system,
                "messages": [{"role": "user", "content": user_msg}],
            },
            timeout=60,
        )
        resp.raise_for_status()
        return resp.json()["content"][0]["text"]
    else:
        resp = httpx.post(
            cfg["endpoint"],
            headers={
                "Authorization": f"Bearer {cfg['api_key']}",
                "Content-Type": "application/json",
            },
            json={
                "model": cfg["model"],
                "max_tokens": 1500,
                "messages": [
                    {"role": "system", "content": system},
                    {"role": "user", "content": user_msg},
                ],
            },
            timeout=60,
        )
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]


def gather_context() -> str:
    """Collect system state for daily review."""
    parts = []

    # Recent heartbeat logs
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    hb_log = SZC_HOME / "logs" / f"heartbeat-{today}.txt"
    if hb_log.exists():
        parts.append(f"## Today's Heartbeat Log\n```\n{hb_log.read_text()[-2000:]}\n```")

    # System snapshot
    uptime = shell("uptime")
    disk = shell("df -h / | tail -1")
    containers = shell("docker ps --format '{{.Names}}: {{.Status}}' 2>/dev/null | head -15")
    parts.append(f"## System Snapshot\n```\nUptime: {uptime}\nDisk: {disk}\n\nContainers:\n{containers}\n```")

    # Telegram session logs
    tg_log = SZC_HOME / "logs" / f"telegram-{today}.txt"
    if tg_log.exists():
        content = tg_log.read_text()
        line_count = content.count("\n")
        parts.append(f"## Today's Telegram Activity\n{line_count} log lines recorded")

    # Memory file
    memory = SZC_HOME / "memory" / "MEMORY.md"
    if memory.exists():
        parts.append(f"## Current Memory\n{memory.read_text()[:2000]}")

    return "\n\n".join(parts)


def send_telegram(message: str):
    if not OWNER_CHAT_ID:
        return
    try:
        httpx.post(
            f"https://api.telegram.org/bot{TELEGRAM_TOKEN}/sendMessage",
            json={"chat_id": OWNER_CHAT_ID, "text": message},
            timeout=10,
        )
    except Exception as e:
        log.error("Telegram send error: %s", e)


def main():
    log.info("Running daily review...")
    cfg = load_config()
    context = gather_context()

    system_prompt = """You are the daily review module for an AI agent.
Your job is to:
1. Analyze today's system health data
2. Check project and pipeline status
3. Identify any patterns or issues
4. Suggest next actions
5. Provide a brief status summary

Keep it under 500 words. Be direct. Use bullet points.
Format for Telegram (no markdown headers, use bullet points and plain text)."""

    user_msg = f"""Generate today's daily review based on this data:

{context}

Provide:
- System health summary (1-2 lines)
- Project/pipeline status
- Any issues or patterns found
- Suggested next actions
- Overall status rating (healthy/warning/critical)"""

    try:
        review = call_anthropic(cfg, system_prompt, user_msg)
        log.info("Review generated: %d chars", len(review))

        # Save to logs
        log_dir = SZC_HOME / "logs"
        log_dir.mkdir(parents=True, exist_ok=True)
        today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        (log_dir / f"daily-review-{today}.txt").write_text(review)

        # Send to Telegram
        header = f"Daily Review - {today}\n{'='*30}\n\n"
        send_telegram(header + review)
        log.info("Daily review sent to Telegram")

    except Exception as e:
        log.error("Daily review failed: %s", e)
        send_telegram(f"Daily review failed: {e}")


if __name__ == "__main__":
    main()
