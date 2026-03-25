"""
SubZeroClaw Telegram Bridge

Uses the shared agent_core module for LLM calls, tools, and conversation.
Same agent logic as SubZeroClaw — unified codebase.
"""

import os
import json
import logging
from pathlib import Path
from datetime import datetime

import httpx
from telegram import Update
from telegram.ext import (
    Application,
    CommandHandler,
    MessageHandler,
    filters,
    ContextTypes,
)
from telegram.constants import ParseMode, ChatAction

from agent_core import create_store
from agent_core.llm import call_llm, load_config, load_system_prompt
from agent_core.tools import execute_tool

logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s: %(message)s",
)
log = logging.getLogger("agent-telegram")

# --- Config ---
TELEGRAM_TOKEN = os.environ["TELEGRAM_BOT_TOKEN"]
ALLOWED_USERS = os.environ.get("TELEGRAM_ALLOWED_USERS", "").split(",")
SZC_HOME = Path(os.environ.get("SZC_HOME", "/agent-data"))
MAX_MSG_LEN = 4096  # Telegram limit

# --- Shared persistent conversation store (Supabase primary, file fallback) ---
store = create_store(fallback_path=SZC_HOME / "conversation.json")


async def agent_loop(chat_id: int, user_message: str, chat=None) -> str:
    """Run the agent loop: LLM call -> tool execution -> repeat until text response."""
    store.append("user", user_message, channel="telegram")

    cfg = load_config()
    system_prompt = load_system_prompt()
    turns = 0
    max_turns = cfg.get("max_turns", 20)

    async def send_typing():
        if chat:
            try:
                await chat.send_action(ChatAction.TYPING)
            except Exception:
                pass

    while turns < max_turns:
        turns += 1
        log.info("Agent turn %d/%d", turns, max_turns)

        await send_typing()
        messages = [{"role": "system", "content": system_prompt}] + store.get_llm_messages()

        try:
            response = await call_llm(messages, cfg)
        except httpx.HTTPStatusError as e:
            log.error("LLM API error: %s", e)
            return f"LLM API error: {e.response.status_code}"
        except Exception as e:
            log.error("LLM call failed: %s", e)
            return f"Error calling LLM: {e}"

        choice = response["choices"][0]
        msg = choice["message"]

        # Check for tool calls
        tool_calls = msg.get("tool_calls")
        if tool_calls:
            store.append_raw(msg, channel="telegram")

            for tc in tool_calls:
                func = tc["function"]
                tool_name = func["name"]
                args = json.loads(func["arguments"])
                await send_typing()

                log.info("TOOL [turn %d] %s: %s", turns, tool_name, str(args)[:150])
                output = await execute_tool(tool_name, args)
                store.append("tool", output, channel="telegram", tool_call_id=tc["id"])

            continue

        # No tool calls — we have a text response
        text = msg.get("content", "")
        store.append("assistant", text, channel="telegram")

        # Auto-compact if conversation is getting long
        max_msgs = cfg.get("max_messages", 40)
        if store.needs_compaction(max_msgs):
            await _auto_compact(cfg)

        # Log the session
        _log_session(chat_id, user_message, text)

        return text

    log.warning("Agent hit max turns (%d) for chat %s", max_turns, chat_id)
    return f"Agent loop completed {max_turns} tool calls without a final response. The task may still be in progress — check the results or try a simpler request."


async def _auto_compact(cfg: dict):
    """Summarize old messages and compact the conversation."""
    context = store.get_compaction_context()
    if not context:
        return
    try:
        summary_msgs = [
            {"role": "system", "content": "Summarize this conversation concisely. Keep key facts, decisions, and context. Be brief."},
            {"role": "user", "content": context},
        ]
        resp = await call_llm(summary_msgs, cfg)
        summary = resp["choices"][0]["message"].get("content", "")
        if summary:
            store.compact(summary)
            log.info("Conversation auto-compacted")
    except Exception as e:
        log.warning("Auto-compact failed: %s", e)


def _log_session(chat_id: int, user_msg: str, assistant_msg: str):
    log_dir = SZC_HOME / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    today = datetime.utcnow().strftime("%Y-%m-%d")
    log_file = log_dir / f"telegram-{today}.txt"
    timestamp = datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S")
    with open(log_file, "a") as f:
        f.write(f"[{timestamp}] CHAT:{chat_id} USER: {user_msg}\n")
        f.write(f"[{timestamp}] CHAT:{chat_id} AGENT: {assistant_msg[:500]}\n\n")


def is_allowed(user_id: int) -> bool:
    if not ALLOWED_USERS or ALLOWED_USERS == [""]:
        return True
    return str(user_id) in ALLOWED_USERS


def split_message(text: str) -> list[str]:
    if len(text) <= MAX_MSG_LEN:
        return [text]
    chunks = []
    while text:
        if len(text) <= MAX_MSG_LEN:
            chunks.append(text)
            break
        split_at = text.rfind("\n", 0, MAX_MSG_LEN)
        if split_at == -1:
            split_at = MAX_MSG_LEN
        chunks.append(text[:split_at])
        text = text[split_at:].lstrip("\n")
    return chunks


# --- Handlers ---

async def start_cmd(update: Update, context: ContextTypes.DEFAULT_TYPE):
    if not is_allowed(update.effective_user.id):
        return
    await update.message.reply_text(
        "MyAgent online.\n"
        "Send me a task or question. I have shell access + MCP integrations.\n"
        "/reset to clear conversation history."
    )


async def reset_cmd(update: Update, context: ContextTypes.DEFAULT_TYPE):
    if not is_allowed(update.effective_user.id):
        return
    store.clear()
    await update.message.reply_text("Conversation cleared.")


async def status_cmd(update: Update, context: ContextTypes.DEFAULT_TYPE):
    if not is_allowed(update.effective_user.id):
        return
    from agent_core.tools import run_shell
    uptime = run_shell("uptime")
    disk = run_shell("df -h / | tail -1")
    containers = run_shell("docker ps --format '{{.Names}}: {{.Status}}' | head -10")
    msg = f"System Status\n\n{uptime}\n\nDisk: {disk}\n\nContainers:\n{containers}"
    await update.message.reply_text(msg)


async def handle_message(update: Update, context: ContextTypes.DEFAULT_TYPE):
    if not is_allowed(update.effective_user.id):
        log.warning("Unauthorized user: %s", update.effective_user.id)
        return

    user_msg = update.message.text
    if not user_msg:
        return

    chat_id = update.effective_chat.id
    log.info("Message from %s: %s", update.effective_user.id, user_msg[:100])

    await update.message.chat.send_action(ChatAction.TYPING)
    response = await agent_loop(chat_id, user_msg, chat=update.message.chat)

    for chunk in split_message(response):
        try:
            await update.message.reply_text(chunk)
        except Exception:
            await update.message.reply_text(chunk[:MAX_MSG_LEN])


def main():
    cfg = load_config()
    log.info("Starting SubZeroClaw Telegram bot...")
    log.info("Model: %s", cfg["model"])
    log.info("Endpoint: %s", cfg["endpoint"])
    log.info("SZC Home: %s", SZC_HOME)
    log.info("Allowed users: %s", ALLOWED_USERS if ALLOWED_USERS != [""] else "all")

    app = Application.builder().token(TELEGRAM_TOKEN).build()

    app.add_handler(CommandHandler("start", start_cmd))
    app.add_handler(CommandHandler("reset", reset_cmd))
    app.add_handler(CommandHandler("status", status_cmd))
    app.add_handler(MessageHandler(filters.TEXT & ~filters.COMMAND, handle_message))

    log.info("Agent bot ready. Polling...")
    app.run_polling(allowed_updates=Update.ALL_TYPES)


if __name__ == "__main__":
    main()
