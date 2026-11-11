# Changelog

XA DevHub changes are listed newest first.

## Unreleased

- Check stable public plugin releases every 30 minutes, using their public manifest download links and matching public changelog entries. The first observation establishes a baseline.
- Remember successful and uncertain announcement attempts across restarts to prevent duplicate posts, and allow definitive rejected sends at a later scheduled check. Configure the release channel on the existing Discord page.
- Add authenticated public release announcements through the existing Discord bot and PowerShell control command, with validated channel IDs, bounded summaries and HTTPS release links.
- Require a Discord message-ID receipt before reporting an announcement as posted. Report definitive failures and uncertain transport outcomes from each submission attempt.
- Disable mentions in public release announcements.

## v1.0.12

- Added a confirmed clear-all action for acknowledged Discord delivery failures, including entries beyond the displayed list.
- Added bulk retry, local alert clearing, and recovery of cleared reaction-cleanup jobs; retries require the bot's Manage Messages permission. Clearing alerts leaves Discord reactions in place.
- Reduced repeated loading of unchanged Discord failure lists while keeping live updates.
- Improved Discord channel-list responsiveness with large message histories.

## v1.0.11

- Added Access Management below Discord Monitors to pin the newest matching admin ZIP upload, replace older matching release pins, and catch up after time offline.

## v1.0.10

- Added a Discord button after Edit and Delete on project ticket rows to open the original captured message; disabled when no source message is available.
- Updated project and Work Queue ticket search to include ticket numbers such as `i224`, alongside titles, details, and contributors.
- Added a sortable Ticket column to Work Queue: `I` for implementations, `F` for fixes, and `O` for other tickets; these labels also work in ticket searches.

## v1.0.9

- Fixed ticket deletion: Delete tickets linked to Discord cards, queue bot-card removal while offline, and prevent replay from recreating deleted work.
- Updated ticket sorting: Remember the selected sort order after reopening DevHub, including resetting to priority / status.
- Fixed build verification: Resolve the Windows shutdown-test helper explicitly and wait for child readiness before testing shutdown.

## v1.0.8

- Added Open tab: View all open project tickets together while retaining search, priority filters, and sorting.
- Added Discord > Commands: Browse bot commands and review actions, or use !xahelp as a whitelisted administrator.
- Added Discord > Bot > Auto Add Channels: Control automatic channel discovery while preserving existing monitor selections.
- Fixed Discord monitoring: Disabled channels stay excluded even when their server is monitored.
- Updated Discord channel names: Refresh missing channel and thread names, including those for disabled monitors.
- Updated Discord card replies: Save notes, files, and images on pending feedback or the linked ticket, with duplicate protection. Approval still requires a project.
- Updated project tickets: Refresh the open project list automatically as feedback changes.
- Improved builds: Reduce repeated backup and test work and display elapsed times for each stage.

## v1.0.7

### Added

- Show completion dates on ticket rows and order tickets by newest or oldest completion date, including the All and Completed tabs.

## v1.0.6

### Added

- Capture authorized Discord mentions as credited tickets in mapped project channels, with project selection for submissions from unmapped channels.
- Add follow-up notes and attachments to an existing ticket by replying to its original Discord message or completed capture post.
- Merge multiple tickets from the same project while preserving their notes, contributors, attachments, and history.
- Rename tickets and merge duplicates through the local API, with checks that prevent outdated requests from overwriting newer changes.
- Search active Discord tickets with `!xatickets <term>` and browse matching titles with previous, next, and close controls.
- Import historical tickets while preserving their IDs, status, dates, contributors, and attachments without overwriting existing tickets.
- Read, create, and update complete project settings through the authenticated local API.
- Create tickets through the local API with duplicate protection for repeated requests.

### Changed

- Separate building, packaging, and FTP staging into three commands, with packaging requiring a verified build that matches the current source.
- Keep FTP staging local by default and make GitHub publication an explicit option.
- Bind local API credentials to the current Windows user and the running DevHub instance.
- Prevent another DevHub instance from taking over the active API port or its credentials.

### Fixed

- Preserve dependency versions when updating the DevHub application version.
- Close a running DevHub window gracefully during local builds, including when the window is hidden.
- Display Discord leaderboard names without unwanted formatting characters.

## v1.0.5 - Initial public source

### Added

- Track projects, fixes, implementations, references, and notes with priorities, due dates, contributors, attachments, and history.
- View project activity, version information, and build pipelines from the dashboard and calendar.
- Collect Discord feedback for review and credit contributors when feedback becomes a ticket.
- Organize research, decisions, development workflows, and reports alongside project work.
- Copy project context and selected tickets into AI assistants for review and implementation.
- Provide an authenticated local API for automation.
- Include installation, update, privacy, contribution, and security guidance.

### Changed

- Start new installations with an editable sample project and no preset administrator or machine-specific settings.
- Keep project data and credentials local, with safeguards against including private files in shared source or packages.
