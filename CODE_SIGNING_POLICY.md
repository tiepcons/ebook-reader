# Code signing policy

**Status:** SignPath Foundation application pending. Until it is approved, releases are unsigned (or carry only a
self-signed alpha certificate that Windows does not trust).

Free code signing provided by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org).

## What is signed

Only the binaries built from this repository:

- `EbookReader.exe`
- `libs\eb_mupdf.dll`

Third-party binaries in the optional Piper pack (`piper.exe`, onnxruntime, espeak-ng and their DLLs) are **not** signed by
this project; they are unmodified downloads pinned by URL and SHA256 in `tools\package.ps1`.

Every signed file carries the same `ProductName` ("Ebook Reader") and `ProductVersion` in its version resource.
Signing requests are restricted to these two files and to that product name.

## How releases are built

- Releases are built only by [`.github/workflows/release.yml`](.github/workflows/release.yml), triggered by a tag `v*`,
  on a GitHub-hosted runner (`windows-2022`) from the tagged commit of this public repository.
- The workflow builds MuPDF and the release preset, runs the tests, packages the zip and submits it to SignPath.
  All actions are pinned by commit SHA.
- Every signing request is manually approved in SignPath by an Approver listed below.
- The GitHub release is created as a draft and published by hand after the signed files have been checked.
- The SignPath artifact configuration is kept in [`.signpath/artifact-configuration.xml`](.signpath/artifact-configuration.xml).

## Team roles

| Role | Member |
|---|---|
| Authors / Committers | [tiepcons](https://github.com/tiepcons) |
| Reviewers | [tiepcons](https://github.com/tiepcons) |
| Approvers | [tiepcons](https://github.com/tiepcons) |

Multi-factor authentication is required for every member with write access to the repository or an account in the
SignPath organization.

## Privacy policy

This program will not transfer any information to other networked systems unless specifically requested by the user or
the person installing or operating it.

- Nothing leaves your machine by default. There is no telemetry, no crash upload and no update check. (The only network
  code in the source is the Azure Speech client in `src\tts\azure`, built on WinHTTP.)
- **Azure Speech (optional, off by default).** Only if you select the Azure Speech engine and enter your own API key, the
  text being read aloud and your key are sent over HTTPS to Microsoft at `<region>.tts.speech.microsoft.com`, where
  `<region>` is the region you entered. See the
  [Microsoft Privacy Statement](https://privacy.microsoft.com/privacystatement). You can switch the engine back or
  delete the key at any time in the voice settings.
- Your books, reading positions, bookmarks and settings stay in the local data folder described below.
- Crash dumps are written locally and contain thread stacks only, no heap memory.

## System changes

Ebook Reader is portable. It does not write to the registry, does not register itself to start with Windows and does not
create file associations. The only persistent items it creates are:

- the data folder: `data\` next to `EbookReader.exe` when that folder is writable (portable mode), otherwise
  `%LOCALAPPDATA%\EbookReader` (library database, covers, settings, logs, crash dumps, speech cache, voices);
- the `ebooks\`, `libs\` and `fonts\` folders next to the exe;
- only if you save an Azure Speech key: one Windows Credential Manager entry named `EbookReader.tiepcons/AzureSpeechKey/v1`.

## Uninstall

1. Close Ebook Reader (also quit it from the tray icon).
2. Delete the application folder.
3. Delete the data folder: `data\` next to the exe (portable mode) or `%LOCALAPPDATA%\EbookReader`.
4. If you used Azure Speech, remove the stored key: `cmdkey /delete:EbookReader.tiepcons/AzureSpeechKey/v1`
   (or delete the entry in Windows Credential Manager).

## Reporting problems

Security issues: see [SECURITY.md](SECURITY.md). Other issues: <https://github.com/tiepcons/ebook-reader/issues>.
