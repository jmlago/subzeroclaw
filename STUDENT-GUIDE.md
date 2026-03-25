# SubZeroClaw -- Student Workshop Guide

Welcome! By the end of this guide, you will have a working AI agent running in Docker that you can chat with on Telegram. The agent will have its own identity, memory, and the ability to call external APIs.

---

## What You Will Build

You will deploy a personal AI agent that:
- Lives in a Docker container on a server (or your laptop)
- Has a unique name, personality, and set of skills you define
- Chats with you through Telegram
- Can run shell commands on the server
- Can call external APIs (weather, Notion, any REST API)
- Remembers things across restarts
- Monitors the server and alerts you proactively

**Time required:** 30-60 minutes for basic setup, plus time for customization.

---

## What You Need Before Starting

Make sure you have ALL of these ready:

- [ ] **Docker Desktop** installed and running ([download](https://docs.docker.com/get-docker/))
- [ ] **A text editor** (VS Code, nano, vim -- anything works)
- [ ] **A Telegram account** (install the app if you don't have it)
- [ ] **A Telegram bot token** -- follow the steps below to get one
- [ ] **An Anthropic API key** -- sign up at [console.anthropic.com](https://console.anthropic.com/)
- [ ] **Your Telegram user ID** -- follow the steps below to get it

### Getting your Telegram bot token

1. Open Telegram and search for `@BotFather`
2. Send `/newbot`
3. Choose a display name (e.g., "My Study Agent")
4. Choose a username (must end in "bot", e.g., `my_study_agent_bot`)
5. BotFather will reply with your token -- it looks like `1234567890:ABCdefGHIjklMNOpqrSTUvwxYZ`
6. Save this token somewhere safe

### Getting your Telegram user ID

1. Open Telegram and search for `@userinfobot`
2. Send any message to it
3. It replies with your user ID -- a number like `123456789`
4. Save this number

---

## Step-by-Step Setup

### Step 1: Get the code

```bash
git clone https://github.com/sergiosegado/subzeroclaw.git my-agent
cd my-agent
```

You should see this structure:
```
my-agent/
  docker-compose.yml
  .env.example
  agent-core/
  telegram-bot/
  proxy/
  data/
    core/       <-- Your agent's identity files
    skills/     <-- Capability definitions
    memory/     <-- Long-term memory
    vault/      <-- API configurations
```

### Step 2: Configure environment variables

```bash
cp .env.example .env
```

Open `.env` in your text editor and fill in:

```
TELEGRAM_BOT_TOKEN=paste-your-bot-token-here
ANTHROPIC_API_KEY=paste-your-anthropic-key-here
TELEGRAM_ALLOWED_USERS=paste-your-telegram-user-id-here
VAULT_KEY=any-random-string-like-this-one
```

**Important:** The `.env` file contains secrets. Never share it or commit it to git.

### Step 3: Name your agent

Open `data/core/IDENTITY.md` in your text editor. Replace the placeholders:

```markdown
## Identity -- StudyBuddy

name: StudyBuddy
alias: Buddy
emoji: (pick any emoji you like)
version: 1.0.0

### Who You Are
You are StudyBuddy -- a study assistant that helps me track assignments
and quiz me on course material. You run on my laptop in Docker.

### Authorized Users
Only these people can talk to you:
- Alex (Telegram ID: 123456789)
```

**Replace** `123456789` with YOUR actual Telegram user ID.

### Step 4: Set personality

Open `data/core/SOUL.md`. Customize the communication style:

```markdown
### Communication Style
- Be encouraging and patient
- Use simple language, avoid jargon
- When explaining concepts, use analogies
- Keep responses under 200 words unless I ask for more detail
```

### Step 5: Build and start

```bash
docker compose up -d --build
```

This builds two containers (bot + proxy) and starts them. First build takes 1-2 minutes.

### Step 6: Copy your data into the container

```bash
docker compose cp data/. agent-bot-1:/agent-data/
```

This copies your identity, skills, memory, and vault config into the running container's data volume.

### Step 7: Check the logs

```bash
docker compose logs -f agent-bot
```

Look for:
```
Starting SubZeroClaw Telegram bot...
Agent bot ready. Polling...
```

If you see errors, check the [Common Mistakes](#common-mistakes-and-how-to-fix-them) section below.

Press `Ctrl+C` to stop following logs (the bot keeps running).

### Step 8: Talk to your agent

1. Open Telegram
2. Search for your bot by its username
3. Send `/start`
4. The bot should reply with a greeting
5. Try sending: "What is your name?" or "What can you do?"

Congratulations -- your agent is running!

---

## Testing Your Agent

Try these messages to verify everything works:

| Message | Expected behavior |
|---------|-------------------|
| `/start` | Bot sends a greeting |
| "What is your name?" | Bot responds with the name from IDENTITY.md |
| "Run: echo hello" | Bot uses the shell tool to run the command |
| "What's your uptime?" | Bot runs `uptime` via shell |
| `/status` | Bot shows system status (disk, memory, containers) |
| `/reset` | Bot clears conversation history |

---

## Adding Your First Skill

Skills teach your agent new things. Let's add a simple one.

### Create a new skill file

Create `data/skills/001-fun-facts.md`:

```markdown
## Skill: Fun Facts

When the user asks for a fun fact or says "tell me something interesting":

1. Use the shell tool to pick a random category:
   shell(command="echo $((RANDOM % 5))")
2. Based on the number, share a fun fact about:
   - 0: Space and astronomy
   - 1: Ocean and marine life
   - 2: History and ancient civilizations
   - 3: Technology and computing
   - 4: Animals and nature
3. Keep the fact to 2-3 sentences
4. Ask if they want another fact
```

### Deploy the skill

```bash
docker compose cp data/skills/001-fun-facts.md agent-bot-1:/agent-data/skills/001-fun-facts.md
docker compose restart agent-bot
```

### Test it

Send your bot: "Tell me a fun fact"

---

## Connecting to an External API

Let's connect your agent to a real API. We'll use a free weather API as an example.

### Step 1: Get a free API key

1. Go to [openweathermap.org](https://openweathermap.org/api)
2. Sign up for a free account
3. Go to "API keys" in your profile
4. Copy your API key

### Step 2: Configure the MCP server

```bash
cp data/vault/mcp-servers.example.json data/vault/mcp-servers.json
```

Edit `data/vault/mcp-servers.json`. Find the weather entry and update it:

```json
{
  "id": "weather",
  "name": "OpenWeatherMap API",
  "description": "Get current weather for any city",
  "enabled": true,
  "target_base_url": "https://api.openweathermap.org/data/2.5",
  "auth_type": "bearer",
  "auth_value": "paste-your-openweathermap-key-here"
}
```

**Important:** Change `"enabled": false` to `"enabled": true`.

### Step 3: Add a weather skill

Create `data/skills/002-weather.md`:

```markdown
## Skill: Weather Checker

When the user asks about weather:

1. Use mcp_request to call the weather API:
   mcp_request(server_id="weather", method="GET", path="/weather?q=CITY_NAME&units=metric")
2. Parse the JSON response
3. Report: temperature (celsius), conditions, humidity, wind speed
4. If the city is not found, ask for the correct spelling
```

### Step 4: Deploy

```bash
docker compose cp data/. agent-bot-1:/agent-data/
docker compose restart agent-bot
```

### Step 5: Test

Send your bot: "What's the weather in Tokyo?"

---

## Customizing Behavior

### Change how the agent responds

Edit `data/core/SOUL.md` to change tone, style, and rules. Re-seed and restart:

```bash
docker compose cp data/core/SOUL.md agent-bot-1:/agent-data/core/SOUL.md
docker compose restart agent-bot
```

### Add more authorized users

Edit `data/core/IDENTITY.md` to add friends:
```markdown
### Authorized Users
- Me (Telegram ID: 123456789)
- My Friend (Telegram ID: 987654321)
```

Also add their IDs to `.env`:
```
TELEGRAM_ALLOWED_USERS=123456789,987654321
```

Then restart: `docker compose restart agent-bot`

### Give the agent memory

Edit `data/memory/MEMORY.md` to pre-load facts:

```markdown
# MEMORY.md

## Key Facts
- My favorite programming language is Python
- I'm studying computer science at Example University
- Current project: building a weather dashboard
```

---

## Common Mistakes and How to Fix Them

### "I sent a message but the bot doesn't reply"

**Most likely:** Your Telegram user ID is wrong or missing from `.env`.

Fix:
1. Double-check your ID with `@userinfobot` on Telegram
2. Make sure `TELEGRAM_ALLOWED_USERS` in `.env` matches exactly (no spaces after commas)
3. Restart: `docker compose restart agent-bot`

### "Container exits immediately"

**Most likely:** Missing or incorrect environment variable.

Fix:
```bash
docker compose logs agent-bot
```
Look for `KeyError: 'TELEGRAM_BOT_TOKEN'` or similar. Make sure all required variables are in `.env`.

### "Agent says it can't use tools"

**Most likely:** Data volume wasn't seeded.

Fix:
```bash
docker compose cp data/. agent-bot-1:/agent-data/
docker compose restart agent-bot
```

### "MCP request returns 404"

**Most likely:** `mcp-servers.json` doesn't exist in the volume.

Fix:
```bash
# Make sure the json file exists locally
ls data/vault/mcp-servers.json

# If not, create it from the example
cp data/vault/mcp-servers.example.json data/vault/mcp-servers.json

# Re-seed
docker compose cp data/vault/mcp-servers.json agent-bot-1:/agent-data/vault/mcp-servers.json
```

### "Agent gives wrong or weird responses"

**Most likely:** Skill descriptions are ambiguous.

Fix: Edit your skill files to be more specific. Instead of "query the database," write:
```
mcp_request(server_id="notion", method="POST", path="/databases/abc123/query", body='{}')
```

### "I edited a file but nothing changed"

**Most likely:** You edited the local file but didn't copy it to the container.

Fix:
```bash
docker compose cp data/. agent-bot-1:/agent-data/
docker compose restart agent-bot
```

---

## Challenge Ideas

Once your basic agent is working, try these:

1. **Multi-skill agent**: Add 3+ skills that work together (e.g., weather + calendar + todo list)
2. **Custom monitoring**: Set up heartbeat checks for a web service you care about
3. **Memory-powered**: Have the agent remember your schedule and remind you of deadlines
4. **API mashup**: Connect two APIs and have the agent combine data from both
5. **Team agent**: Add multiple authorized users and give the agent team-specific knowledge
6. **Daily digest**: Configure the daily review to send you a morning briefing
7. **Custom safety rules**: Add domain-specific safety rules (e.g., "never delete rows from the production database")
8. **Conversation personality**: Create an agent with a specific persona (a pirate, a professor, a coach) and see how SOUL.md shapes behavior

---

## Quick Reference

| Command | What it does |
|---------|-------------|
| `docker compose up -d --build` | Build and start containers |
| `docker compose cp data/. agent-bot-1:/agent-data/` | Seed/update agent data |
| `docker compose logs -f agent-bot` | Follow bot logs |
| `docker compose logs -f agent-proxy` | Follow proxy logs |
| `docker compose restart agent-bot` | Restart bot after changes |
| `docker compose down` | Stop containers (keeps data) |
| `docker compose down -v` | Stop and DELETE all data |
| `docker compose ps` | Check container status |

| Telegram command | What it does |
|-----------------|-------------|
| `/start` | Initialize the bot |
| `/reset` | Clear conversation history |
| `/status` | Show system status |

---

*Built with SubZeroClaw Agent Framework.*
