# XA DevHub

XA DevHub is a native Windows application that keeps software projects, tickets, Discord feedback, build commands, research, and AI handoffs in one place. Your project data stays in a folder you control, with no hosted account or telemetry.

**Platform:** 64-bit Windows 10 or newer. **License:** [AGPL-3.0-or-later](LICENSE).

**Start with the [complete DevHub guide on XA Docs](https://xadocs.com/devhub/)** for setup, everyday use, Discord integration, project pipelines, AI workflows, and backups.

## What you can do

- Track fixes, implementations, references, and notes with priorities, contributors, attachments, and completion history.
- Browse all open project tickets in the Open tab, alongside Completed and All, while keeping the same search, priority filters, and sorting.
- See each completed ticket's completion date and order project tickets by newest or oldest completion in the All or Completed tab, with tickets without a completion date listed last.
- Compare local and public project versions and run your saved Build, Prep, Release, and Version commands.
- Review Discord feedback, approve tickets, credit contributors, and retain follow-up evidence.
- Keep durable knowledge, development workflows, reports, and resumable AI handoffs alongside your projects.
- Back up your data and export portable Markdown or JSON.

## Install and get started

Get started by [building from source](CONTRIBUTING.md) and following the [DevHub guide](https://xadocs.com/devhub/). Portable binary packages, when available, will appear under [GitHub Releases](https://github.com/xa-io/devhub/releases).

The numbered build workflow reports stage timings and runs the complete application tests without repeating overlapping domain suites. Source backups skip excluded build and dependency folders before scanning their contents.

To install a portable package:

1. Download the portable ZIP and its `SHA256SUMS.txt`, then verify the ZIP's SHA-256 against the published checksum.
2. Extract the entire ZIP to a writable folder, keeping every DLL beside `devhub.exe`.
3. Run `devhub.exe`, edit the starter **My Project** entry, and add your first ticket.
4. Follow the [DevHub guide](https://xadocs.com/devhub/) to configure the features you want.

If Windows reports a missing `MSVCP140.dll` or `VCRUNTIME140.dll`, install the [Microsoft Visual C++ 2015–2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).

To update, close DevHub, back up the complete `data\` folder, and replace the application files while retaining your data. If you use a custom data directory, back up that directory instead. To uninstall, close DevHub, keep any data or exports you need, and delete the application folder.

## Companion AI skills

Get the DevHub skills from [XA Docs](https://xadocs.com/skills/) or download the [complete DevHub skills bundle](https://xadocs.com/downloads/devhub/xa-devhub-skills.zip). The website provides installation instructions, readable skill source, and individual downloads; this repository contains the application source.

The skills help agents register projects, create and organize tickets, check implementations, maintain knowledge, coordinate development, and prepare reports through DevHub's authenticated local API. See the [guide](https://xadocs.com/devhub/) for choosing and using them.

## Build from source

You need Visual Studio 2022 with **Desktop development with C++**, CMake 3.20 or newer, Python 3, Git, and vcpkg. Follow [CONTRIBUTING.md](CONTRIBUTING.md) for an isolated contributor build and validation steps.

The numbered `1. build.py` wrapper builds the production checkout and can stop and restart that checkout's running DevHub after backing up its data. Read its help before using it. Build output is `build\Release\devhub.exe`.

## Your data

DevHub stores project data beside the executable in `data\` by default. Keep this folder private and back up attachment folders with the database. Portable exports can contain project text and contributor details even when secret settings are omitted; review them before sharing. Read [PRIVACY.md](PRIVACY.md) for the full data policy.

## Contributing and security

- [Contributing](CONTRIBUTING.md): development setup and pull-request guidance.
- [Security policy](SECURITY.md): how to report vulnerabilities privately.
- [Changelog](CHANGELOG.md): public-facing change history.
- [License](LICENSE), [project notice](NOTICE), and [third-party notices](THIRD_PARTY_NOTICES.md): distribution terms and attribution.
