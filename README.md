# SubZeroClaw

**197 lines of C. 58KB binary. A skill-driven agentic daemon for edge hardware.**

```
skill.md + LLM + shell + loop = autonomous agent
```

Every agentic runtime does the same thing: read a skill, call an LLM, execute tools, loop. SubZeroClaw is that principle written directly in C — no framework, no abstractions, no architecture mimicking a problem that never existed. One file, one loop, three tools.

## What it does

You write a skill as a markdown file. You point SubZeroClaw at it. It calls an LLM, executes tools, loops until done. That's the entire runtime.

```
~/.subzeroclaw/skills/monitor.md    ← what the agent knows
~/.subzeroclaw/config               ← API key + model
~/.subzeroclaw/logs/<session>.txt   ← full I/O trace
```

The agent reads the skill into its system prompt, receives input, and autonomously calls tools until the task is complete. When context gets full, it compacts old messages into a summary and keeps going.

## Why not just use ZeroClaw / OpenClaw?

[ZeroClaw](https://github.com/zeroclaw-labs/zeroclaw) rewrites [OpenClaw](https://github.com/openclaw) in Rust. It's good software — but it inherits the architecture of the thing it's replacing: trait systems, channel adapters, observer patterns, identity formats, security layers. All solutions to problems that exist when you're building a multi-user, multi-channel platform.

If your problem is "run one skill on one Pi", none of that applies. You don't need channel adapters because there's one channel. You don't need a security model because you wrote the skill. You don't need a trait system because there's one provider.

SubZeroClaw doesn't simplify their architecture. It ignores it and writes the loop directly.

|                   | SubZeroClaw  | ZeroClaw     | OpenClaw     |
|-------------------|--------------|--------------|--------------|
| Language          | C            | Rust         | TypeScript   |
| Source            | 197 lines    | ~15,000      | ~430,000     |
| Binary            | 58 KB        | 3.4 MB       | 80+ MB       |
| RAM (runtime)     | ~14 MB       | < 5 MB       | 80-120 MB    |
| Compiles on Pi    | 0.5s         | OOM          | slow         |
| Dependencies      | curl, cJSON  | ~100 crates  | ~800 npm     |

## Tools

Three tools. The shell makes every CLI program on the system available to the LLM:

| Tool | What it does |
|------|-------------|
| **shell** | `popen()` any command |
| **read_file** | Read file contents |
| **write_file** | Write/create files, auto `mkdir -p` |

Since the LLM has a shell, it has `git`, `curl`, `himalaya`, `signal-cli`, `ffmpeg`, `jq`, `khal`, `pass` — whatever you install. No adapters, no integrations. The adapter is the shell.

## Skills

Drop a `.md` file in `~/.subzeroclaw/skills/`. It becomes part of the system prompt.

```bash
cat > ~/.subzeroclaw/skills/backup.md << 'EOF'
## Backup Agent
You monitor /home/pi/data every hour.
- Run `rsync -avz /home/pi/data pi@nas:/backup/`
- If rsync fails, retry 3 times with 30s delay
- Log results to /home/pi/backup.log
EOF
```

No format spec. No skill registry. No trigger matching. Just plain text the LLM reads.

## Build

```bash
make            # builds subzeroclaw (58KB)
make watchdog   # builds watchdog (17KB)
make test       # runs 14 tests
make install    # copies to ~/.local/bin/
```

Requires `libcjson-dev` or uses vendored cJSON automatically.

## Setup

```bash
mkdir -p ~/.subzeroclaw/skills

cat > ~/.subzeroclaw/config << EOF
api_key = "sk-or-your-openrouter-key"
model = "anthropic/claude-sonnet-4-20250514"
EOF
```

Environment variables override the config file:

```
SUBZEROCLAW_API_KEY
SUBZEROCLAW_MODEL
SUBZEROCLAW_ENDPOINT
```

## Usage

```bash
# One-shot task
./subzeroclaw "check disk usage and clean tmp if over 80%"

# Interactive
./subzeroclaw

# Daemon with watchdog (restarts on crash, exponential backoff)
./watchdog ./subzeroclaw "run the backup skill"
```

## Session logging

Every session gets a random hex ID. All input, output, tool calls, and results are logged to `~/.subzeroclaw/logs/<session>.txt` with timestamps.

```
=== f850c58ddd4ae72a Sun Feb 16 16:30:01 2026
[2026-02-16 16:30:01] USER: check disk usage
[2026-02-16 16:30:03] TOOL: shell
[2026-02-16 16:30:03] RES: /dev/sda1  72% /
[2026-02-16 16:30:04] ASST: Disk usage is at 72%, below threshold.
```

## Context compaction

When the message history exceeds `max_messages` (default 40), the agent:

1. Sends old messages to the LLM for summarization
2. Replaces them with the summary
3. Keeps the last N raw messages intact

No vector DB. No embeddings. One API call to compress context.

## Config reference

| Key | Default | Description |
|-----|---------|-------------|
| `api_key` | (required) | OpenRouter API key |
| `model` | `anthropic/claude-sonnet-4-20250514` | Any OpenAI-compatible model |
| `endpoint` | `https://openrouter.ai/api/v1/chat/completions` | API endpoint |
| `skills_dir` | `~/.subzeroclaw/skills` | Path to skill markdown files |
| `log_dir` | `~/.subzeroclaw/logs` | Session log directory |
| `max_turns` | 50 | Max tool-call loops per input |
| `max_messages` | 40 | Trigger context compaction |
| `compact_keep` | 16 | Raw messages to keep after compaction |

## Source

```
src/
├── subzeroclaw.c   197 lines   The entire runtime
├── test.c                      14 tests
├── watchdog.c       47 lines   Crash recovery + backoff
├── cJSON.c                     Vendored JSON parser
└── cJSON.h
```

## Philosophy

The intelligence is in the LLM, not in the runtime. The runtime's only job is to give the model a skill and a shell, then get out of the way.

OpenClaw solved this with 430,000 lines of TypeScript. ZeroClaw re-solved it with 15,000 lines of Rust. Both are good — but both carry the weight of problems that only exist at platform scale: multi-tenancy, channel routing, identity portability, plugin registries.

SubZeroClaw asks: what if the problem is just "one agent, one skill, one device"? Then the answer is 197 lines of C.

## License

MIT
