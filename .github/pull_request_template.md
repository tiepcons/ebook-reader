## What and why

<!-- One logical change. Task ID from docs/PLAN.md if any (e.g. T2.4). -->

## Checklist

- [ ] Tests added/updated (corrupt-input tests for parsing code)
- [ ] `tools\test.ps1 -Preset debug` and `-Preset asan` pass
- [ ] `tools\build.ps1 -Preset lint` passes
- [ ] No new third-party dependency (or discussed in an issue first)
- [ ] Functions ≤ 50 lines, files ≤ 600 lines, no dead code
