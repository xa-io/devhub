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

Current source version: **1.0.6**

Platform: **64-bit Windows 10 or newer**

License: **[AGPL-3.0-or-later](LICENSE)**

> Download official portable packages from the [GitHub Releases page](https://github.com/xa-io/devhub/releases), or follow the [source-build instructions](#build-from-source) to build locally.

## Preparing a release

The local maintainer workflow has three separate commands: `python "1. build.py"` builds and verifies the application; `python "2. Prepare_release.py"` packages that verified build; `python "3. Push_release.py"` lists the complete source tree, portable files, compressed entries, and upload assets, then stages the four FTP files under `ftp-upload/v1.0.6/downloads/xa-devhub/`. The prepare and push helpers are private operator tools and are excluded from the public source checkout. Public contributors can build the application with step 1.

Step 2 requires a successful build receipt matching the current source and runtime files. It never builds or encrypts anything. Step 3 uses the exact prepared files and never connects to FTP. Upload the ZIP, `SHA256SUMS.txt`, and `release-manifest.json` to `/downloads/xa-devhub/`, then upload `latest.json` last. These are the same four files used as GitHub release assets.

Step 3 defaults to local staging. `--dry-run` prints the full candidate without copying files; `--source-only` audits source before a build exists. Future `--publish` use requires a GitHub write token in `XA_GITHUB_WRITE_TOKEN`, `GH_TOKEN`, or `GITHUB_TOKEN`, normal Git push credentials, and two typed terminal confirmations after the complete preview. It checks that local `main` matches remote `main`, refuses existing release tags, revalidates the candidate, commits the reviewed source, pushes `main:main`, and uploads assets to a draft before publishing it. A failed upload leaves the release as a draft for inspection.

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

To install a portable package:

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
From the ticket editor, operators can merge several unmerged same-project
tickets into one retained active ticket in a single atomic action. DevHub
appends every saved title and note, moves contributors, attachments, and
Discord lineage to the retained ticket, and keeps each source `I<id>` as an
immutable merged audit record. Source rows include their current status, so
completed feedback can still be consolidated without reopening it first.
The authenticated local API exposes the same cleanup primitives as strict
title-only and canonical batch-merge operations. Both bind every affected
ticket to the exact `updated_at` value reviewed by the caller; stale plans fail
without applying a partial cleanup.
Project pages provide a plain-text critical/normal/low priority filter, group P4
and P3 under critical, and can order tickets by newest/oldest submitted date.

The Dashboard compares a project's detected local version with an optional
published version source:

- **blue:** local development is ahead;
- **green:** local and published versions match;
- **red:** the local version is older than the published version;
- **local:** no published version source is configured.

The Dashboard completion timeframe defaults to 30 days. Settings can change it
to 7, 30, 60, 90, 180, or 365 days, and the Dashboard count and compact label
use the same saved selection.

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
suggestions, and place them in a review inbox. Automatically detected feedback
becomes a ticket only after approval. An authorized administrator's direct bot
mention is different: in a channel with an exact active project mapping it is
promoted immediately at normal P2 priority; in a common or unmapped channel it
stays pending so an operator can choose the project. A direct entry classified
as a bug by the configured detector becomes a fix; other direct entries become
implementations. The first mentioned non-bot user other than DevHub receives
contributor attribution, with the submitting administrator as the fallback.
Promotion credits that contributor atomically; there is no second credit action after promotion.

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
`!leaderboard`. On a pending review card, use the warning to approve as high (P3),
the check mark to approve at normal priority, the sleepy reaction to approve as
low priority, or X to reject. **Edit pending** changes the proposed note and,
when selected, the destination project before promotion.

`!xatickets` without an argument keeps the compact active-project summary.
`!xatickets <term>` performs a case-insensitive substring search across the
titles and bodies of active fix/implementation tickets in Discord-enabled
projects. The response exposes only each ticket ID, project, and title; titles
are shortened safely to 50 Unicode characters. Search cards show 20 results per
page and, when needed, use left/right reactions for paging plus X to close.
Only configured administrators/developers can run the command or change pages.

For later notes or evidence, mention DevHub while replying to either the original
source or a completed admin capture post. DevHub follows the durable command
lineage back to the same ticket, appends the follow-up exactly once, and keeps
the ticket's current status.

Projects appear in ticket menus only when that option is enabled and the project
has active fix or implementation work. Projects with zero active tickets are omitted.
Compact leaderboard rows use plain contributor names so Discord's global
Markdown escaping cannot display literal formatting markers. After a review card
reaches a terminal state, DevHub uses Discord's Delete All Reactions endpoint to
remove every admin or user's emoji from that exact bot-owned card; pending and
unrelated cards remain untouched.

### Attachments and deletion behavior

Pending suggestions store attachment metadata, not attachment bytes. When an
authorized action promotes or captures the message, DevHub refetches and
validates the files before saving them locally. A ticket accepts up to 10
attachments, 10 MiB per file, and 50 MiB in total.
Validated images are stored under the ticket image tree. Other Discord files use
opaque generated names under `<data-dir>\ticket-files\I<item-id>\` with an inert
`.bin` suffix.

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
WAL/SHM files, logs, credentials, attachments, exports, and rotating backups.
The rotating local-API credential is kept outside project data in the current
Windows profile's non-roaming LocalAppData, as described below. Project data may
therefore remain on a local or user-configured mapped path without inheriting
that storage location's identity model for API-token discovery.

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

When the DevHub Codex integration is installed, eight focused skills cover the
common workflows. Four form the core packet and lifecycle layer:

- `devhub-control` resolves the selected live loopback instance, safely
  transports authenticated reads and mutations, and verifies writes by reading
  them back.
- `devhub-development` manages intent, development phases, checkpoints,
  decisions, evidence, validation, and resumable handoffs.
- `devhub-knowledge` captures, searches, links, reconciles, and audits durable
  knowledge.
- `devhub-reports` creates and governs evidence-grounded reports and release
  drafts.

Four focused helpers cover common entry and review tasks:

- `devhub-add-project` discovers conservative metadata, prevents duplicates,
  and registers a local directory with complete project configuration and
  readback verification.
- `devhub-create-ticket` turns evidence into one bounded, duplicate-checked
  DevHub ticket, previews the mutation, and verifies the saved item by ID.
- `devhub-cleanup-tickets` reviews one project's active queue for clearer
  title-only names and true duplicate requests, presents stable proposal IDs
  for human approval, and applies only the approved renames and canonical
  merges without completing or deleting tickets.
- `devhub-implementation-confirmer` performs a read-only evidence check of
  tickets in a copied AI review bundle and separates implemented work from work
  that is still outstanding.

Example:

```text
$devhub-add-project C:\Projects\MyApplication
$devhub-cleanup-tickets XA Slave
```

The DevHub skills use the API. They never need direct access to `devhub.db`.
Repository or domain skills remain responsible for the actual code; DevHub
supplies durable context and evidence without expanding the user's authority.

## Local automation API

DevHub's API is intended for local automation only. It binds to loopback on
port `21100` by default. Before starting Crow, it holds one machine-wide lease
for the selected port so a second DevHub instance cannot reuse the socket or
rotate the active token. After it owns that port, it atomically publishes a
current-user-only `xa-devhub.api-rendezvous/v1` record at
`%LOCALAPPDATA%\XA DevHub\api\api-token-v1-<port>.json`. The record binds the
canonical `http://127.0.0.1:<port>` origin, process ID, and rotating token to the
exact running instance and is removed on an owned clean shutdown. Every request
requires `X-DevHub-Token` and a literal loopback `Host`. Supported clients
canonicalize `localhost`, `127.0.0.1`, and `::1`, validate the record, and fail
before making a request when discovery is missing, malformed, stale, or for the
wrong instance. Do not expose the API to a public network or print, commit, or
share the token.

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
GET/POST  /api/work
GET       /api/work/<id>
POST      /api/work/<id>/title
POST      /api/work/<id>/merge
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

Exact project configuration reads and full-field saves use the same complete
native-editor contract for authenticated create and update operations.
Project pipeline requests select only saved project commands and working
directories; callers cannot inject a replacement command or `cwd`. Mutating
requests reject browser origins, require JSON, and enforce bounded request
bodies. Project configuration saves require the complete native-editor field
set and are written atomically.

Work creation requires a complete project-bound payload (`project_id`, type,
title, body, P1-P4 priority, due/review dates, and tags). New items are always
open with API origin. Exact active replays are idempotent; different content
under an existing active title is a conflict rather than an overwrite.
Title updates accept only `title` and `expected_updated_at`. Batch merges accept
one target timestamp plus 1-200 distinct source IDs and timestamps, require one
project, preserve every source as a readable `merged` audit row, and return the
target and sources for direct verification. These routes never complete or
delete a ticket.

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
exact production executable is already running with the default no-argument
command line, it backs up the live database and requests `WM_CLOSE` from the
one PID-owned `XADevHubNative` top-level window even when Windows reports that
window hidden. Missing, ambiguous, unqueueable, or still-running window states
stop the build without a force-kill. The app restarts only after compile, CTest,
self-test, and disposable migration validation succeed. The wrapper preserves
`build\Release\data` and limits native build concurrency to four jobs or a
lower limit supplied by the caller. Runtime health succeeds only after the
wrapper validates that the LocalAppData rendezvous belongs to the exact process
it launched.

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
