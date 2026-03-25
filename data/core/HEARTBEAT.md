## Heartbeat -- Proactive Monitoring for [YOUR_AGENT_NAME]

### System Health (every 30 min)

- Check disk usage -- flag if > 85%
- Check memory pressure -- flag if > 80%
- Check Docker containers for restart loops or unhealthy status

### Application Checks (every 1h)

- Verify MCP proxy is reachable: curl -s http://agent-proxy:9090/health
- [YOUR_CHECK: Example: curl -s https://YOUR_SERVICE_URL/health]
- [YOUR_CHECK: Example: Check if YOUR_DATABASE has fewer than N pending jobs]
- [YOUR_CHECK: Example: Verify SSL certificate for YOUR_DOMAIN is valid]
- [ADD OR REMOVE CHECKS AS NEEDED]

### Custom Checks

[FILL IN: Add checks specific to your use case]

Examples:
- Check if a specific API returns 200: curl -s -o /dev/null -w "%{http_code}" https://[YOUR_API_URL]/health
- Count records in a database: mcp_request to [YOUR_SERVER_ID]
- Verify a cron job ran today: check logs in /agent-data/logs/
- Monitor a file for changes: stat /path/to/[YOUR_FILE]

### Agent Self-Check (every cycle)

- Context message count -- if > 30, trigger compaction
- Check /agent-data/logs/ for errors
- Verify MCP servers are reachable

### Escalation Rules

- Critical (services down, disk full): message owner immediately via Telegram
- Warning (high usage, degraded performance): note in daily summary
- Info (clean runs, routine checks): log only, do not message
