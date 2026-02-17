# Contributing to SubZeroClaw

SubZeroClaw is an anti-framework. The goal is not to grow — it's to stay minimal while remaining readable and correct. Contributions are welcome, but they should go in one of two directions:

## 1. Reduce complexity

If you can make SubZeroClaw do the same thing with less code, fewer moving parts, or clearer logic — without sacrificing readability — that's a contribution.

Examples of good PRs:
- Removing a function that doesn't earn its place
- Simplifying a control flow path
- Eliminating a dependency or an allocation
- Replacing a hand-rolled pattern with something the C standard library already provides

Examples of bad PRs:
- Adding a feature "just in case"
- Introducing an abstraction for something that happens once
- Compressing readable code into fewer lines at the expense of clarity
- Adding configuration for something that works fine hardcoded

The code is currently ~380 lines. Every line should justify its existence. If yours removes lines while keeping tests green, it's probably good.

## 2. Prove a limitation of the anti-framework

SubZeroClaw's thesis is: a skill file + a shell + an LLM loop is enough to do anything an agentic runtime needs to do. If you can demonstrate a task where this breaks down — and the problem is in the runtime, not in the skill — that's valuable.

To be clear about the distinction:

- **Skill problem**: "The agent can't send email well." That's a skill authoring issue. Write a better `email.md` that teaches the model how to use `himalaya` correctly. That's not a SubZeroClaw limitation.
- **Runtime problem**: "The agent can't handle binary tool output." That's a real structural limitation — `popen` returns text, the message protocol is JSON strings, there's no way to pass a binary blob back to the model. That's a contribution worth discussing.

If you find a runtime limitation, open an issue with:
1. The task you tried to accomplish
2. The skill you wrote (paste the `.md`)
3. What happened vs. what should have happened
4. Why the skill can't work around it (why the runtime itself needs to change)

This forces an honest conversation: does SubZeroClaw actually need to grow, or does the skill just need to be better?

## What we won't merge

- New tools. The shell is the only tool. If the model needs `git`, it runs `git`. If it needs to write a file, it runs `tee`. Adding dedicated tools is the first step toward becoming the framework this project exists to avoid.
- Plugin systems, hook mechanisms, event buses, or middleware. These solve multi-user platform problems. SubZeroClaw is one agent, one skill, one device.
- Backward-compatibility shims. If something changes, it changes. The user base is small enough to just update.

## Running tests

```bash
make test
```

All 16 tests must pass. If you change `subzeroclaw.c`, update `test.c` to match.
