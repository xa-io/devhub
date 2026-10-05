# Changelog

XA DevHub changes are listed newest first.

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
