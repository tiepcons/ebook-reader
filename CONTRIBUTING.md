# Contributing

Thanks for your interest! / Cảm ơn bạn đã quan tâm!

Ebook Reader is a small, fast, crash-resistant native Windows ebook reader. Contributions that keep it that way are welcome.

## Before you start

- Read [docs/PLAN.md](docs/PLAN.md). It has the scope, the decisions (D1…), and the **code rules in §4**.
- For anything bigger than a bug fix, open an issue first so we can agree on the approach.
- **No new third-party dependencies** without prior discussion (PLAN.md D14).

## Build and test

Requirements: Windows 10/11 x64, Visual Studio 2022 with *Desktop development with C++* and *C++ Clang tools*, Git.

```powershell
git clone --recurse-submodules https://github.com/tiepcons/ebook-reader.git
cd ebook-reader
tools\test.ps1  -Preset debug     # build + unit/integration tests
tools\test.ps1  -Preset asan      # same under AddressSanitizer
tools\build.ps1 -Preset lint      # clang-tidy, warnings are errors
tools\build.ps1 -Preset release
```

The first build compiles MuPDF (a few minutes); later builds skip it.

## Pull requests

1. One logical change per PR. Reference the task ID from PLAN.md if there is one (e.g. `T2.4`).
2. Tests first: add or update doctest cases under `tests/`. New parsing code also needs corrupt-input tests.
3. All of `debug`, `asan` and `lint` must pass. No new warnings, no `NOLINT` without a reason in the comment.
4. Follow the existing style (`.clang-format`, `.editorconfig`): C++23, `namespace eb`, `Result<T>` for errors, RAII for every handle,
   no exceptions across C / DLL / WndProc / thread boundaries, functions ≤ 50 lines, files ≤ 600 lines.
5. Commit messages: imperative mood, prefixed with the task ID when it applies (`T2.4: extract EPUB cover from OPF`).

## Test corpus

Only add public-domain or permissively licensed books to `tests/corpus/`, and record the source and license in
`tests/corpus/SOURCES.md`. Keep files small (≤ 5 MB).

## License

By contributing you agree that your contribution is licensed under the [AGPL-3.0](LICENSE).
