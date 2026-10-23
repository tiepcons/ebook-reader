# Security Policy

Ebook Reader opens untrusted files (EPUB, PDF, MOBI/AZW3, FB2, CBZ, TXT), so parser bugs are treated as security issues.

## Reporting a vulnerability

**Please do not open a public issue.** Report privately via
[GitHub Security Advisories](https://github.com/tiepcons/ebook-reader/security/advisories/new)
or by email to tiep.pnh@vnsb.dev.

Include the version or commit, the file that triggers the issue (or how to craft it), and what happens
(crash, hang, excessive memory, file access outside the library folder, …).

You can expect an acknowledgement within 7 days. Fixes are released as soon as practical and credited unless you prefer otherwise.

## Supported versions

Only the latest release and `main` receive security fixes.

## Hardening already in place

- MuPDF and libmobi run behind a C ABI with a hard heap cap, input-size limits and cancellable rendering.
- Unused document handlers (JS, XPS, plain HTML, images, Markdown) are compiled out.
- Builds use `/guard:cf`, `/sdl`, `/CETCOMPAT`, ASLR; DLLs are loaded only from the app's `libs\` folder.
- Crash dumps contain stacks only (no heap), so they never include book text or API keys.
- The optional Azure Speech key is stored in Windows Credential Manager, never in files or logs.
