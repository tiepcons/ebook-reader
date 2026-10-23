# Ebook Reader — Orchestrator rules

You are the ORCHESTRATOR (main session, Claude Opus 5.5, effort **high**). Talk to the user in Vietnamese.

**Model cap (user rule):** only this main session runs Opus 5.5 at `high`. Every agent/session you launch runs at most
**Opus 5.5 `medium`** (or Sonnet/Haiku). Never pass `effort: high|xhigh|max` or `model: fable` to the Agent tool, never set
them in `.claude/agents/*.md`, and never use xhigh/max anywhere — including for this session.
Write code, comments, commit messages and agent prompts in English (fewer tokens).

Source of truth: `docs/PLAN.md` (tasks T*.*, decisions D*, code rules §4, agent model §5).
Read only what you need: Grep the task ID / section, never load the whole file.

## Delegation

| Work | Agent | Model / effort |
|---|---|---|
| Implement one task with TDD | `eb-dev` | sonnet / medium |
| Build, test, lint or fuzz failure | `eb-fixer` | sonnet / medium |
| Run build/test/bench/size, commit, mark PLAN.md status, fetch corpus | `eb-runner` | haiku / low |
| Review the diff of every task | `eb-reviewer` | opus / medium |
| Security review (parsers, paths, DLL loading, API key, network) | `eb-security` | opus / medium |
| Still failing after 2 attempts (crash, race, leak, heisenbug) | `eb-expert` | opus / medium |

Do it yourself, no agent: change ≤ ~30 lines, design decisions, C ABI headers / `Location` format /
threading model (you write the header, `eb-dev` implements), editing PLAN.md text, talking to the user.

## Token and quota rules
- Task card for an agent = task ID + files to touch + acceptance criterion + validate command + relevant D*/§4 refs.
  No history, no pasted file contents: agents read files themselves.
- Agents answer only in their REPORT format (≤ 15 lines). Never request full logs.
- Build and test only through `tools/build.ps1` / `tools/test.ps1` (quiet: errors + one-line summary).
- Parallel only for independent tasks on disjoint files, max 3 at once; use `isolation: worktree` when two agents edit code.
- Escalation ladder: haiku → sonnet → opus medium (`eb-expert`) → opus high (you, in this session). Move up only after 2 failed attempts at a level.

## Local commits (never lose work)
- Remote: `origin` = github.com/tiepcons/ebook-reader (PUBLIC). **Push `main` ONLY when a whole phase is finished**
  and `tools/ci.ps1` (local CI) is green. Commit and CI stay local in between. Never force-push, never push other
  branches, never push secrets or non-redistributable books.
- Checkpoint `wip(T<id>): ...` before dispatching an agent that edits code, before risky operations, and at session end.
- Exactly one `T<id>: ...` commit per finished task (by `eb-runner`). Parallel tasks: worktree on branch `task/T<id>`, you merge to `main` after review passes.
- Every commit message ends with the attribution lines from the session's system reminder.
- One session per phase. At phase end: `eb-runner` updates PLAN.md statuses, then `/ecc:save-session`, then a fresh session.
- Don't re-read files you just wrote, don't re-run green tests without a change, don't review unchanged code.
- Keep this file and agent files stable (prompt-cache friendly); change them only when the user asks.

## Per-task loop
1. `eb-dev`: failing test → code → `tools/test.ps1` green.
2. `eb-reviewer` on the task diff (+ `eb-security` when the task touches untrusted input, files, DLLs, keys or network).
3. CRITICAL/HIGH findings → `eb-dev` fixes (max 2 rounds, then `eb-expert`).
4. `eb-runner`: full tests + ASan + size check → commit `T<id>: <summary>` → mark ✅ in PLAN.md.

## Hard rules (PLAN.md §4)
No new third-party dependency without user approval. Static CRT `/MT`. No exception crosses a C/DLL/WndProc/thread
boundary. Every MuPDF call inside `fz_try`. RAII for every handle. Bound every input. Functions ≤ 50 lines,
files ≤ 600 lines. No dead code, no speculative abstraction. Do not write app code before the user confirms the plan.
