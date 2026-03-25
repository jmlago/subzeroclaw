"""
Supabase-backed conversation store for SubZeroClaw.

Uses PostgREST via Kong gateway — no extra dependencies beyond httpx.
Falls back to file-based store if Supabase is unavailable.
"""

import json
import logging
import os
from datetime import datetime, timezone

import httpx

log = logging.getLogger("szc.supabase_store")


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


class SupabaseConversationStore:
    """Supabase PostgreSQL-backed conversation store via PostgREST."""

    def __init__(self, supabase_url: str = None, service_key: str = None, max_messages: int = 200):
        self.base_url = (supabase_url or os.environ.get("SUPABASE_URL", "")).rstrip("/")
        self.service_key = service_key or os.environ.get("SUPABASE_SERVICE_KEY", "")
        self.max_messages = max_messages
        self._headers = {
            "apikey": self.service_key,
            "Authorization": f"Bearer {self.service_key}",
            "Content-Type": "application/json",
            "Prefer": "return=representation",
        }

    @property
    def available(self) -> bool:
        return bool(self.base_url and self.service_key)

    def _url(self, path: str) -> str:
        # Support both Kong gateway (/rest/v1/) and direct PostgREST (/) URLs
        if "/rest/v1" in self.base_url:
            return f"{self.base_url}/{path}"
        elif ":3000" in self.base_url or ":3001" in self.base_url:
            # Direct PostgREST connection — no /rest/v1 prefix
            return f"{self.base_url}/{path}"
        else:
            return f"{self.base_url}/rest/v1/{path}"

    def _sync_get(self, path: str, params: dict = None) -> list:
        try:
            with httpx.Client(timeout=10) as c:
                resp = c.get(self._url(path), headers=self._headers, params=params or {})
                resp.raise_for_status()
                return resp.json()
        except Exception as e:
            log.warning("Supabase GET %s failed: %s", path, e)
            return []

    def _sync_post(self, path: str, data: list | dict) -> list:
        try:
            with httpx.Client(timeout=10) as c:
                resp = c.post(self._url(path), headers=self._headers, json=data)
                resp.raise_for_status()
                return resp.json()
        except Exception as e:
            log.warning("Supabase POST %s failed: %s", path, e)
            return []

    def _sync_delete(self, path: str, params: dict = None):
        try:
            with httpx.Client(timeout=10) as c:
                resp = c.delete(self._url(path), headers=self._headers, params=params or {})
                resp.raise_for_status()
        except Exception as e:
            log.warning("Supabase DELETE %s failed: %s", path, e)

    def get_history(self) -> list[dict]:
        rows = self._sync_get("szc_messages", {
            "select": "*",
            "order": "created_at.asc",
            "limit": str(self.max_messages),
        })
        return [self._row_to_msg(r) for r in rows]

    def get_display_history(self) -> list[dict]:
        return self.get_history()

    def append(self, role: str, content, channel: str = "dashboard", **extra):
        row = {
            "role": role,
            "content": content if isinstance(content, str) else json.dumps(content),
            "channel": channel,
            "is_summary": extra.get("is_summary", False),
            "tool_call_id": extra.get("tool_call_id"),
            "metadata": json.dumps({k: v for k, v in extra.items() if k not in ("is_summary", "tool_call_id", "tool_calls")}),
        }
        if extra.get("tool_calls"):
            row["tool_calls"] = json.dumps(extra["tool_calls"])
        self._sync_post("szc_messages", row)
        self._enforce_limit()

    def append_raw(self, msg: dict, channel: str = "dashboard"):
        row = {
            "role": msg.get("role", "assistant"),
            "content": msg.get("content", "") or "",
            "channel": channel,
        }
        if msg.get("tool_calls"):
            row["tool_calls"] = json.dumps(msg["tool_calls"])
        if msg.get("tool_call_id"):
            row["tool_call_id"] = msg["tool_call_id"]
        self._sync_post("szc_messages", row)
        self._enforce_limit()

    def get_llm_messages(self) -> list[dict]:
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
        self._sync_delete("szc_messages", {"id": "gt.0"})

    def needs_compaction(self, max_context_messages: int = 40) -> bool:
        rows = self._sync_get("szc_messages", {"select": "id", "limit": str(max_context_messages + 1)})
        return len(rows) > max_context_messages

    def compact(self, summary: str):
        # Get IDs of all but last 10
        rows = self._sync_get("szc_messages", {"select": "id", "order": "created_at.asc"})
        if len(rows) <= 10:
            return
        old_ids = [r["id"] for r in rows[:-10]]
        # Delete old messages
        for i in range(0, len(old_ids), 50):
            batch = old_ids[i:i+50]
            ids_param = ",".join(str(x) for x in batch)
            self._sync_delete("szc_messages", {"id": f"in.({ids_param})"})
        # Insert summary
        self._sync_post("szc_messages", {
            "role": "system",
            "content": f"[Conversation summary from earlier messages]\n{summary}",
            "channel": "system",
            "is_summary": True,
        })

    def get_compaction_context(self) -> str:
        rows = self._sync_get("szc_messages", {"select": "*", "order": "created_at.asc"})
        if len(rows) <= 10:
            return ""
        old = rows[:-10]
        parts = []
        for r in old:
            role = r.get("role", "?")
            content = r.get("content", "")
            if content:
                channel = r.get("channel", "")
                prefix = f"[{channel}] " if channel else ""
                parts.append(f"{prefix}{role}: {content[:500]}")
        return "\n".join(parts)

    def _enforce_limit(self):
        """Delete oldest messages if over limit."""
        rows = self._sync_get("szc_messages", {"select": "id", "order": "created_at.asc"})
        if len(rows) > self.max_messages:
            excess = len(rows) - (self.max_messages // 2)
            old_ids = [r["id"] for r in rows[:excess]]
            for i in range(0, len(old_ids), 50):
                batch = old_ids[i:i+50]
                ids_param = ",".join(str(x) for x in batch)
                self._sync_delete("szc_messages", {"id": f"in.({ids_param})"})

    def _row_to_msg(self, row: dict) -> dict:
        msg = {
            "role": row.get("role", ""),
            "content": row.get("content", ""),
            "channel": row.get("channel", "dashboard"),
            "timestamp": row.get("created_at", _now()),
        }
        if row.get("tool_calls"):
            tc = row["tool_calls"]
            msg["tool_calls"] = json.loads(tc) if isinstance(tc, str) else tc
        if row.get("tool_call_id"):
            msg["tool_call_id"] = row["tool_call_id"]
        if row.get("is_summary"):
            msg["is_summary"] = True
        if row.get("metadata"):
            meta = row["metadata"]
            if isinstance(meta, str):
                try:
                    meta = json.loads(meta)
                except Exception:
                    meta = {}
            if meta:
                msg.update(meta)
        return msg


def create_store(supabase_url: str = None, service_key: str = None, fallback_path=None, max_messages: int = 200):
    """Create the best available store: Supabase if configured, else file-based."""
    supa = SupabaseConversationStore(supabase_url, service_key, max_messages)
    if supa.available:
        # Quick connectivity check
        try:
            supa._sync_get("szc_messages", {"select": "id", "limit": "1"})
            log.info("Using Supabase conversation store at %s", supa.base_url)
            return supa
        except Exception as e:
            log.warning("Supabase store unavailable (%s), falling back to file", e)

    if fallback_path:
        from .conversation import ConversationStore
        log.info("Using file-based conversation store at %s", fallback_path)
        return ConversationStore(fallback_path, max_messages)

    raise RuntimeError("No conversation store available: Supabase not configured and no fallback path")
