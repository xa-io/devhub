# XA DevHub

XA DevHub is a local-first Windows application for keeping software projects,
work items, feedback, build commands, research, and AI handoffs in one place.
It is meant for developers who have outgrown scattered text files, chat logs,
and half-remembered release steps but do not want to move private project data
into a hosted service.

The app is native C++ using Dear ImGui, DirectX 11, and Win32. Discord support
runs inside the application, and an authenticated loopback API is available for
local automation. There is no browser shell, hosted account, telemetry, or XA
cloud sync.

Current source version: **1.0.5**

Platform: **64-bit Windows 10 or newer**

License: **[AGPL-3.0-or-later](LICENSE)**

> **Release status:** no public build has been published yet. When one is
> available, the official portable packages will be on the
> [GitHub Releases page](https://github.com/xa-io/devhub/releases). Until then,
> use the [source-build instructions](#build-from-source).

## What DevHub helps you do

- See every project's active work, version state, and configured pipeline from
  one dashboard.
- Track fixes, implementations, references, and notes without losing their
  history, contributors, evidence, due dates, or review state.
- Turn Discord feedback into credited tickets while keeping approval under an
  administrator's control.
- Save durable knowledge, decisions, source-backed claims, development
  workflows, verification evidence, and reports.
- Copy bounded, self-contained packets into Codex for review, implementation,
  reporting, or later continuation.
- Keep the application and its data portable: DevHub stores its runtime state
  in a folder you control.

## Get started

### Install a portable release

Once a release is available:

1. Download `XA-DevHub-vX.Y.Z-windows-x64.zip` and `SHA256SUMS.txt` from the
   Releases page.
2. Verify the ZIP's SHA-256 against the published checksum.
3. Extract the entire ZIP to a writable folder. Keep all DLLs beside
   `devhub.exe`; do not run it from inside the ZIP.
4. Run `devhub.exe`.

If Windows reports that `MSVCP140.dll` or `VCRUNTIME140.dll` is missing,
install the
[Microsoft Visual C++ 2015-2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).
Published builds are portable and may be unsigned, so Windows can display an
unknown-publisher warning. Download builds only from the repository's Releases
page.

To update, close DevHub, back up `data\`, and replace the application files
while retaining the existing `data\` folder. To uninstall, close DevHub,
preserve any backups or exports you want, and delete the application folder.
DevHub does not install a machine-wide service.

### Your first ten minutes

1. **Open DevHub.** A fresh database contains one path-free starter project
   named **My Project**. Edit it or delete it and create your own.
2. **Add a project.** Enter its name and local folder. Repository URL, aliases,
   version sources, Codex skill routes, and pipeline commands are optional.
3. **Add work.** Open the project and use **quick add: title...**. DevHub creates
   the ticket and opens its full editor for details, priority, status,
   contributors, dates, and notes.
4. **Configure commands if useful.** A project can expose Build, Prep, Release,
   and Version actions. DevHub runs only the commands saved for that project.
5. **Set your backup policy.** Open **Settings > Backups and portable exports**
   to choose retention, create a backup, or export data.
6. **Add integrations only if wanted.** Discord monitoring, the local API, and
   Codex workflows are optional; ordinary project and ticket tracking works
   without them.

The second launch focuses the existing DevHub window instead of opening another
copy.

## Find your way around

| Page | What it is for |
| --- | --- |
| **Dashboard** | Project-level workload, versions, recent completions, pending Discord feedback, credits, and configured pipeline actions. |
| **Work Queue** | A cross-project view filtered by project, type, status, priority, overdue, stale, review-due, blocked, or text. |
| **Project** | Fixes, implementations, references, notes, contributors, evidence, release notes, and full ticket editing. |
| **Discord** | Bot connection, monitored servers/channels, suggestion review, notification cards, and delivery failures. |
| **Knowledge & Processing** | Durable knowledge, context search, conflicts, reports, development workflows, and health checks. |
| **Calendar** | Completions, releases, due dates, events, and pipeline runs. |
| **Credits** | Contributor attribution, shipped counts, identity correction, and a public-friendly leaderboard. |
| **Settings** | App preferences, display name, data location, API details, backup retention, backups, and exports. |

### Projects and work items

Each work item has a permanent `I<id>` identity. Tickets can record status,
priority, blocked reasons, due and review dates, multiple contributors,
attachments, release notes, and completion history. Duplicate feedback can be
merged without discarding attribution. An active, unmerged ticket can also move
to another active project without changing its identity or evidence lineage.

The Dashboard compares a project's detected local version with an optional
published version source:

- **blue:** local development is ahead;
- **green:** local and published versions match;
- **red:** the local version is older than the published version;
- **local:** no published version source is configured.

Build, Prep, Release, and Version buttons appear only when their commands are
configured. Release actions require an explicit confirmation. Automatic
version changes affect only the final numeric component by one and never carry
or borrow into an earlier component; use manual entry to change earlier parts.

### AI review and release packets

The two project copy actions serve different moments in the work:

- **Copy AI review bundle** sends selected active tickets and their bounded
  evidence to a new AI session. Right-click an active ticket to copy just that
  item, or use the project button to choose a larger set.
- **Copy release draft** sends recently completed ticket IDs, completion times,
  and contributors as grounded material for release notes. It does not version,
  publish, upload, or deploy anything.

Packets include their scope, hashes, skill routing, workspace context, and an
explicit authority boundary. Pasting a packet supplies context; it does not
silently authorize edits or publication.

## Optional Discord feedback

DevHub can monitor selected Discord channels, detect likely bugs or
suggestions, and place them in a review inbox. A suggestion becomes a ticket
only after approval. The original author is then attached as a contributor and
credited when the work ships.

### Set up the bot

1. Create an application and bot in the Discord developer portal.
2. Enable **MESSAGE CONTENT INTENT**. Give the bot read and history access in
   each channel DevHub should monitor.
3. In **Discord > Bot**, paste the token and select **Save token + connect**.
4. Add administrator/developer Discord user IDs to the whitelist. These must be
   numeric user IDs, not usernames.
5. Add a server ID to monitor the whole server, or a channel ID to monitor only
   that channel.
6. Optionally choose a notification channel for review cards. **Manage
   Messages** is required there if DevHub should remove reactions from finished
   cards.

DevHub backfills messages posted while it was closed and periodically rechecks
monitored channels. Authorized users can review notification cards with
reactions, capture follow-up replies, and use `!tickets`, `!xatickets`, and
`!leaderboard`. Projects appear in ticket menus only when that option is enabled
and the project has active work.

### Attachments and deletion behavior

Pending suggestions store attachment metadata, not attachment bytes. When an
authorized action promotes or captures the message, DevHub refetches and
validates the files before saving them locally. A ticket accepts up to 10
attachments, 10 MiB per file, and 50 MiB in total.

DevHub never edits or deletes the original suggestion message. When Discord
content disappears, DevHub scrubs private source text but can retain minimal
identity and delivery markers needed to prevent a delayed event from recreating
it. Approved ticket evidence and contributor history have their own lifecycle.

## Knowledge, workflows, and reports

These record types solve different problems:

| Use | Store it as |
| --- | --- |
| A bug, feature request, or action someone must complete | **Work item** |
| A fact, decision, design, procedure, research result, or reusable lesson | **Knowledge** |
| Multi-step development that needs intent, checkpoints, evidence, and a safe resume point | **Workflow** |
| A dated interpretation of selected records, such as a weekly or release report | **Report** |

The native **Quick capture** form is suitable for ordinary knowledge. Give the
record a self-contained title, future-reader context, summary, body, tags, and
freshness:

- `timeless` for stable facts or definitions;
- `snapshot` for something observed at a specific time;
- `pointer` for a link to a source that should remain authoritative.

For evidence-grade research, the Codex integration can capture immutable source
text and hashes, add claim-level locators, link supporting or contradicting
records, reconcile conflicts, and verify the saved result. Stale knowledge is a
prompt to recheck the evidence, not an instruction to erase history.

**Copy Codex context** can produce four progressively deeper packets. L0 is a
scope and result summary, L1 adds record identities, L2 adds the normal
future-reader context and summaries, and L3 adds bounded bodies, claims,
sources, locators, dates, and hashes.

## Local data, privacy, and backups

By default, DevHub creates `data\` beside `devhub.exe`. A custom location can be
selected with `--data-dir`. The folder can contain the SQLite database and its
WAL/SHM files, logs, a per-process API token, credentials, attachments, exports,
and rotating backups.

- DevHub has no analytics or telemetry and no XA-hosted cloud account.
- Network access occurs only for features you configure, such as Discord,
  published-version checks, or local automation clients.
- Credential-shaped settings are protected with Windows DPAPI for the current
  Windows profile.
- Portable JSON and Markdown exports omit secret settings, but they can still
  contain private project, contributor, Discord, knowledge, build, and evidence
  text. Review every export before sharing it.
- A database backup includes attachment records but not the sibling attachment
  files. Back up `ticket-images\` and `ticket-files\` separately when the files
  must travel with the database.
- Deleting a ticket removes its database attachment records but intentionally
  leaves already-saved files for manual retention or cleanup.

DevHub creates a coherent database backup and portable exports once per day.
Use **Settings > Backups and portable exports** for retention, **Back up now**,
**Export now**, and quick access to their folders.

Read [PRIVACY.md](PRIVACY.md) before sharing diagnostics, databases, exports,
or screenshots containing real project data.

## Codex integration

When the DevHub Codex integration is installed, six focused skills cover the
common workflows. Four form the core packet and lifecycle layer:

- `devhub-control` safely transports authenticated reads and mutations through
  the localhost API and verifies writes by reading them back.
- `devhub-development` manages intent, development phases, checkpoints,
  decisions, evidence, validation, and resumable handoffs.
- `devhub-knowledge` captures, searches, links, reconciles, and audits durable
  knowledge.
- `devhub-reports` creates and governs evidence-grounded reports and release
  drafts.

Two focused helpers cover common entry and review tasks:

- `devhub-add-project` discovers conservative metadata, prevents duplicates,
  and registers a local directory with complete project configuration and
  readback verification.
- `devhub-implementation-confirmer` performs a read-only evidence check of
  tickets in a copied AI review bundle and separates implemented work from work
  that is still outstanding.

Example:

```text
$devhub-add-project C:\Projects\MyApplication
```

The DevHub skills use the API. They never need direct access to `devhub.db`.
Repository or domain skills remain responsible for the actual code; DevHub
supplies durable context and evidence without expanding the user's authority.

## Local automation API

DevHub's API is intended for local automation only. It binds to loopback on
port `21100` by default and requires the rotating token written to
`data\api-token`. Every request requires `X-DevHub-Token` and a literal loopback
`Host`. Do not expose the API to a public network or print, commit, or share the
token.

The API covers health, projects, work, builds, Discord ingest, AI packet
exports, knowledge, conflicts, reports, workflows, learnings, processing runs,
and retrieval evaluations. Use `devhub-control` when possible so authentication,
validation, mutation preview, and post-write readback are consistent.

<details>
<summary>Common endpoint groups</summary>

```text
GET       /api/health
GET/POST  /api/projects
GET/POST  /api/projects/<id>
GET       /api/work
GET       /api/work/<id>
POST      /api/work/<id>/status
POST      /api/work/import
POST      /api/projects/<id>/build
GET       /api/builds/<id>
GET/POST  /api/discord/channels
POST      /api/discord/ingest
GET       /api/export/review
GET       /api/export/release
GET/POST  /api/knowledge
GET/POST  /api/knowledge/conflicts
GET       /api/knowledge/health
GET       /api/knowledge/context
GET/POST  /api/reports
GET/POST  /api/workflows
GET/POST  /api/learnings
GET/POST  /api/processing-runs
GET/POST  /api/retrieval-evaluations
```

Project pipeline requests select only saved project commands and working
directories; callers cannot inject a replacement command or `cwd`. Mutating
requests reject browser origins, require JSON, and enforce bounded request
bodies. Project configuration saves require the complete native-editor field
set and are written atomically.

</details>

### Import selected historical tickets

The authenticated `POST /api/work/import` route, also exposed by the DevHub
control client as `work-import`, can add explicitly selected ticket IDs from a
separate schema-v14 data directory. The import is additive only: it does not
update or delete destination tickets. It preflights project identity, ticket
history, contributor and Discord lineage, completion events, and saved
attachment hashes before writing the batch.

Keep a historical `devhub.db` with its WAL/SHM companions and attachment folders
in one coherent, stopped copy. Use this feature for deliberate reconciliation,
not as a substitute for ordinary backups.

## Command-line options

```text
devhub.exe                  Open the native application
devhub.exe --headless       Run the API and Discord monitor without a window
devhub.exe --port 21100     Select a different loopback port
devhub.exe --data-dir D:\x  Select a different data directory
devhub.exe --selftest       Run built-in disposable checks and exit
devhub.exe --check-db PATH  Migrate and integrity-check a stopped or copied DB
```

`--selftest` creates temporary data and a temporary loopback port. It does not
open the configured production database or connect the Discord bot.

## Build from source

### Requirements

- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.20 or newer
- Python 3
- Git and vcpkg
- Network access during the first dependency restore

The normal production entry point is:

```powershell
python "1. build.py" --help
python "1. build.py"
```

The wrapper creates a filtered source snapshot, configures a Visual Studio 2022
x64 build with vcpkg, builds Release, and runs its validation gates. If its
exact production executable is already running, it can back up the live
database, close that process, validate the new database migration and runtime,
and restart only after success. It preserves `build\Release\data` and limits
native build concurrency to four jobs or a lower limit supplied by the caller.

Output: `build\Release\devhub.exe`

For an isolated contributor build and the equivalent manual CMake commands,
see [CONTRIBUTING.md](CONTRIBUTING.md). The built-in `devhub.exe --selftest`
remains available in public source builds even though the maintainers' larger
local regression workspace is not published.

## Contributing and security

- Read [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, validation,
  and pull-request expectations.
- Report vulnerabilities privately using [SECURITY.md](SECURITY.md); never put
  real tokens, databases, Discord IDs, private project text, or attachments in
  a public issue.
- See [CHANGELOG.md](CHANGELOG.md) for public-facing changes,
  [PRIVACY.md](PRIVACY.md) for data behavior, and
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for dependency licenses.

## Repository layout

```text
include/devhub/       Shared public headers
src/                  Core project, suggestion, ticket, and version logic
src/app/              Native UI, database, Discord, API, packets, and lifecycle
third_party/licenses/ Dependency license texts
tools/                Public repository and icon utilities
vcpkg-overlays/       Repository-owned dependency overlays
```
