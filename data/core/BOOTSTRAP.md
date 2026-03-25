## Bootstrap -- First-Run Checklist for [YOUR_AGENT_NAME]

When the agent starts for the first time, verify each item:

### Infrastructure Checks

1. MCP proxy is reachable: `curl -s http://agent-proxy:9090/health`
2. Docker socket is mounted: `docker ps` works
3. Logs directory exists: `/agent-data/logs/`
4. Memory file exists: `/agent-data/memory/MEMORY.md`
5. Config file loads correctly

### Custom Checks

[FILL IN: Add first-run checks specific to your setup]

Examples:
- Verify [YOUR_API_SERVICE] is reachable via MCP proxy
- Check that [YOUR_DATABASE] is accessible
- Confirm [YOUR_EXTERNAL_TOOL] credentials are valid
- [ADD YOUR OWN FIRST-RUN CHECKS]

### Post-Bootstrap

- Send a startup message to owner: "[YOUR_AGENT_NAME] online. All systems checked."
- Log startup to /agent-data/logs/startup.txt
- [OPTIONAL: Run an initial data sync or cache refresh]
- [OPTIONAL: Send a summary of system status]

### If Any Check Fails

- Report the specific failure to the owner via Telegram
- Continue operating with available capabilities
- Log the failure for later investigation
