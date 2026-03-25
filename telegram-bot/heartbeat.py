"""
SubZeroClaw Heartbeat — Proactive Monitoring

Runs on a schedule (cron or loop) to check system health.
Only sends Telegram alerts for warnings and critical issues.
Logs all findings to /agent-data/logs/heartbeat-YYYY-MM-DD.txt
"""

import os
import json
import subprocess
import logging
from datetime import datetime
from pathlib import Path

import httpx

logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s: %(message)s")
log = logging.getLogger("agent-heartbeat")

TELEGRAM_TOKEN = os.environ["TELEGRAM_BOT_TOKEN"]
OWNER_CHAT_ID = os.environ.get("TELEGRAM_ALLOWED_USERS", "").split(",")[0]
SZC_HOME = Path(os.environ.get("SZC_HOME", "/agent-data"))
LOG_DIR = SZC_HOME / "logs"


def shell(cmd: str, timeout: int = 30) -> str:
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return (r.stdout + r.stderr).strip()
    except subprocess.TimeoutExpired:
        return f"TIMEOUT after {timeout}s"
    except Exception as e:
        return f"ERROR: {e}"


def check_disk() -> list[dict]:
    """Check disk usage."""
    findings = []
    output = shell("df -h / | tail -1")
    parts = output.split()
    if len(parts) >= 5:
        usage_pct = int(parts[4].replace("%", ""))
        if usage_pct >= 95:
            findings.append({"level": "critical", "msg": f"Disk usage CRITICAL: {usage_pct}% ({parts[3]} free)"})
        elif usage_pct >= 85:
            findings.append({"level": "warning", "msg": f"Disk usage high: {usage_pct}% ({parts[3]} free)"})
    return findings


def check_memory() -> list[dict]:
    """Check memory usage."""
    findings = []
    output = shell("free -m | grep Mem")
    parts = output.split()
    if len(parts) >= 7:
        total = int(parts[1])
        used = int(parts[2])
        pct = (used / total * 100) if total > 0 else 0
        if pct >= 90:
            findings.append({"level": "critical", "msg": f"Memory CRITICAL: {pct:.0f}% ({used}MB/{total}MB)"})
        elif pct >= 80:
            findings.append({"level": "warning", "msg": f"Memory high: {pct:.0f}% ({used}MB/{total}MB)"})
    return findings


def check_containers() -> list[dict]:
    """Check Docker container health."""
    findings = []
    output = shell("docker ps --format '{{.Names}} {{.Status}}'")
    for line in output.strip().split("\n"):
        if not line.strip():
            continue
        parts = line.split(" ", 1)
        name = parts[0]
        status = parts[1] if len(parts) > 1 else ""
        if "Restarting" in status:
            findings.append({"level": "critical", "msg": f"Container {name} is restarting: {status}"})
        elif "unhealthy" in status.lower():
            findings.append({"level": "warning", "msg": f"Container {name} unhealthy: {status}"})
    return findings


def check_load() -> list[dict]:
    """Check system load average."""
    findings = []
    output = shell("cat /proc/loadavg")
    parts = output.split()
    if parts:
        try:
            load_1m = float(parts[0])
            cpu_count = int(shell("nproc") or "2")
            if load_1m > cpu_count * 2:
                findings.append({"level": "critical", "msg": f"Load average CRITICAL: {load_1m} (CPUs: {cpu_count})"})
            elif load_1m > cpu_count:
                findings.append({"level": "warning", "msg": f"Load average high: {load_1m} (CPUs: {cpu_count})"})
        except (ValueError, IndexError):
            pass
    return findings


def check_docker_disk() -> list[dict]:
    """Check Docker disk usage."""
    findings = []
    reclaim_output = shell("docker system df --format '{{.Reclaimable}}'")
    for line in reclaim_output.strip().split("\n"):
        line = line.strip()
        if "GB" in line:
            try:
                val = float(line.replace("GB", "").strip().split("(")[0].strip())
                if val > 10:
                    findings.append({"level": "warning", "msg": f"Docker has {val:.1f}GB reclaimable space. Consider `docker system prune`."})
            except (ValueError, IndexError):
                pass
    return findings


def send_telegram(message: str):
    """Send a message to the owner via Telegram."""
    if not OWNER_CHAT_ID:
        log.warning("No TELEGRAM_ALLOWED_USERS set, cannot send heartbeat alert")
        return
    try:
        resp = httpx.post(
            f"https://api.telegram.org/bot{TELEGRAM_TOKEN}/sendMessage",
            json={"chat_id": OWNER_CHAT_ID, "text": message},
            timeout=10,
        )
        if resp.status_code != 200:
            log.error("Telegram send failed: %s", resp.text)
    except Exception as e:
        log.error("Telegram send error: %s", e)


def log_findings(findings: list[dict]):
    """Write findings to daily log file."""
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    today = datetime.utcnow().strftime("%Y-%m-%d")
    log_file = LOG_DIR / f"heartbeat-{today}.txt"
    timestamp = datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S")

    with open(log_file, "a") as f:
        if not findings:
            f.write(f"[{timestamp}] OK — all checks passed\n")
        else:
            for finding in findings:
                f.write(f"[{timestamp}] [{finding['level'].upper()}] {finding['msg']}\n")


def main():
    log.info("Running heartbeat checks...")
    findings = []
    findings.extend(check_disk())
    findings.extend(check_memory())
    findings.extend(check_load())
    findings.extend(check_containers())
    findings.extend(check_docker_disk())

    log_findings(findings)

    # Only alert on warnings and criticals
    criticals = [f for f in findings if f["level"] == "critical"]
    warnings = [f for f in findings if f["level"] == "warning"]

    if criticals:
        msg = "CRITICAL ALERT\n\n"
        for f in criticals:
            msg += f"- {f['msg']}\n"
        if warnings:
            msg += "\nWarnings:\n"
            for f in warnings:
                msg += f"- {f['msg']}\n"
        send_telegram(msg)
        log.info("Sent critical alert to Telegram")
    elif warnings:
        msg = "Warning\n\n"
        for f in warnings:
            msg += f"- {f['msg']}\n"
        send_telegram(msg)
        log.info("Sent warning alert to Telegram")
    else:
        log.info("All checks passed — no alerts needed")


if __name__ == "__main__":
    main()
