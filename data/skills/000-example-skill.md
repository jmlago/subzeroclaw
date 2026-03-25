## Skill: Notion Database Query

This skill teaches the agent how to query a Notion database via the MCP proxy.

### Setup Required

- MCP server with id "notion" must be configured in data/vault/mcp-servers.json
- Database ID: [YOUR_NOTION_DATABASE_ID]

### Query all entries (paginated)

```
mcp_request(
  server_id="notion",
  method="POST",
  path="/databases/[YOUR_NOTION_DATABASE_ID]/query",
  body='{"page_size": 100}'
)
```

### Search by name

```
mcp_request(
  server_id="notion",
  method="POST",
  path="/databases/[YOUR_NOTION_DATABASE_ID]/query",
  body='{"filter": {"property": "Name", "rich_text": {"contains": "[SEARCH_TERM]"}}}'
)
```

### Filter by status

```
mcp_request(
  server_id="notion",
  method="POST",
  path="/databases/[YOUR_NOTION_DATABASE_ID]/query",
  body='{"filter": {"property": "Status", "select": {"equals": "Active"}}}'
)
```

### When to use

- When asked "how many records?" -> query with no filter, count results
- When asked about a specific record -> search by name, then get page content
- When asked to update a record -> PATCH /pages/[PAGE_ID] with new properties

---

## Skill: Weather Checker

This skill teaches the agent how to check weather using the OpenWeatherMap API.

### Setup Required

- MCP server with id "weather" must be configured in data/vault/mcp-servers.json
- OpenWeatherMap API key must be set as the auth_value

### Get current weather for a city

```
mcp_request(
  server_id="weather",
  method="GET",
  path="/weather?q=[CITY_NAME]&units=metric"
)
```

### Get 5-day forecast

```
mcp_request(
  server_id="weather",
  method="GET",
  path="/forecast?q=[CITY_NAME]&units=metric&cnt=5"
)
```

### When to use

- When the user asks "what's the weather in [city]?"
- When the user asks for a forecast
- Parse the JSON response and report: temperature, conditions, humidity, wind speed
- If the city is not found, ask the user to check the spelling

### Error handling

- If the API returns 401: API key is invalid, report to owner
- If the API returns 404: city not found, suggest alternatives
- If the API times out: try again once, then report the issue

---

## Skill: Custom REST API

This skill is a template for connecting to any REST API via the MCP proxy.

### Setup Required

- MCP server with id "[YOUR_API_ID]" must be configured in data/vault/mcp-servers.json
- API base URL: [YOUR_API_BASE_URL]

### List all items

```
mcp_request(
  server_id="[YOUR_API_ID]",
  method="GET",
  path="/[YOUR_ENDPOINT]"
)
```

### Get a specific item by ID

```
mcp_request(
  server_id="[YOUR_API_ID]",
  method="GET",
  path="/[YOUR_ENDPOINT]/[ITEM_ID]"
)
```

### Create a new item

```
mcp_request(
  server_id="[YOUR_API_ID]",
  method="POST",
  path="/[YOUR_ENDPOINT]",
  body='{"name": "[ITEM_NAME]", "description": "[ITEM_DESCRIPTION]"}'
)
```

### Update an item

```
mcp_request(
  server_id="[YOUR_API_ID]",
  method="PUT",
  path="/[YOUR_ENDPOINT]/[ITEM_ID]",
  body='{"name": "[UPDATED_NAME]"}'
)
```

### Delete an item

```
mcp_request(
  server_id="[YOUR_API_ID]",
  method="DELETE",
  path="/[YOUR_ENDPOINT]/[ITEM_ID]"
)
```

### When to use

- When the user asks to list, view, create, update, or delete items in [YOUR_SERVICE_NAME]
- Always confirm before DELETE operations
- Parse the JSON response and present results clearly
