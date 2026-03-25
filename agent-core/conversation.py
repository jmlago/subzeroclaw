"""
Shared persistent conversation store for SubZeroClaw.

All channels (dashboard WebSocket, Telegram) read/write to the same
JSON file so conversation history is unified and persistent.
"""

import json
import fcntl
import time
from pathlib import Path
from datetime import datetime, timezone


class ConversationStore:
    """File-backed conversation store with file locking for concurrency."""

    def __init__(self, path: Path, max_messages: int = 200):
        self.path = path
        self.max_messages = max_messages
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if not self.path.exists():
            self._write({"messages": [], "updated_at": _now()})

    def _read(self) -> dict:
        try:
            with open(self.path, "r") as f:
                fcntl.flock(f, fcntl.LOCK_SH)
                data = json.load(f)
                fcntl.flock(f, fcntl.LOCK_UN)
                return data
        except (json.JSONDecodeError, FileNotFoundError):
            return {"messages": [], "updated_at": _now()}

    def _write(self, data: dict):
        with open(self.path, "w") as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            json.dump(data, f, indent=2, default=str)
            fcntl.flock(f, fcntl.LOCK_UN)

    def get_history(self) -> list[dict]:
        """Return all messages as LLM-compatible history."""
        data = self._read()
        return data.get("messages", [])

    def get_display_history(self) -> list[dict]:
        """Return messages with metadata for UI display."""
        data = self._read()
        return data.get("messages", [])

    def append(self, role: str, content, channel: str = "dashboard", **extra):
        """Append a message. Content can be str or dict (for tool_calls)."""
        data = self._read()
        msg = {
            "role": role,
            "content": content,
            "channel": channel,
            "timestamp": _now(),
        }
        msg.update(extra)
        data["messages"].append(msg)
        data["updated_at"] = _now()

        # Compact if over limit — keep recent messages
        if len(data["messages"]) > self.max_messages:
            data["messages"] = data["messages"][-self.max_messages // 2:]

        self._write(data)

    def append_raw(self, msg: dict, channel: str = "dashboard"):
        """Append a raw LLM message dict (for tool_calls etc)."""
        data = self._read()
        msg["channel"] = channel
        msg["timestamp"] = msg.get("timestamp", _now())
        data["messages"].append(msg)
        data["updated_at"] = _now()

        if len(data["messages"]) > self.max_messages:
            data["messages"] = data["messages"][-self.max_messages // 2:]

        self._write(data)

    def get_llm_messages(self) -> list[dict]:
        """Return messages formatted for LLM API (strip metadata)."""
        messages = []
        for msg in self.get_history():
            llm_msg = {"role": msg["role"]}
            if msg.get("tool_calls"):
                llm_msg["tool_calls"] = msg["tool_calls"]
                llm_msg["content"] = msg.get("content", "")
            elif msg.get("tool_call_id"):
                llm_msg["tool_call_id"] = msg["tool_call_id"]
                llm_msg["content"] = msg.get("content", "")
            else:
                llm_msg["content"] = msg.get("content", "")
            messages.append(llm_msg)
        return messages

    def clear(self):
        """Clear all messages."""
        self._write({"messages": [], "updated_at": _now()})

    def needs_compaction(self, max_context_messages: int = 40) -> bool:
        """Check if conversation needs compaction."""
        return len(self.get_history()) > max_context_messages

    def compact(self, summary: str):
        """Replace old messages with a summary, keep recent ones."""
        data = self._read()
        messages = data.get("messages", [])
        recent = messages[-10:] if len(messages) > 10 else messages
        compacted = [{
            "role": "system",
            "content": f"[Conversation summary from earlier messages]\n{summary}",
            "channel": "system",
            "timestamp": _now(),
            "is_summary": True,
        }] + recent
        data["messages"] = compacted
        data["updated_at"] = _now()
        data["last_compaction"] = _now()
        self._write(data)

    def get_compaction_context(self) -> str:
        """Get older messages as text for summarization."""
        messages = self.get_history()
        if len(messages) <= 10:
            return ""
        old = messages[:-10]
        parts = []
        for m in old:
            role = m.get("role", "?")
            content = m.get("content", "")
            if isinstance(content, str) and content:
                channel = m.get("channel", "")
                prefix = f"[{channel}] " if channel else ""
                parts.append(f"{prefix}{role}: {content[:500]}")
        return "\n".join(parts)


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()
