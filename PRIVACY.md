# Privacy

XA DevHub is a local-first Windows application. It does not provide an XA-hosted
cloud account or synchronization service, and it does not include analytics or
telemetry. Network activity occurs only for features the operator configures,
including Discord, project version sources, and local automation clients.

## Data stored locally

By default, DevHub stores runtime data in the `data` folder beside
`devhub.exe`. Depending on the enabled features, that folder can contain:

- projects, work items, notes, knowledge, reports, evidence, and workflows;
- contributor names, stable Discord identities, message metadata, and review
  state;
- saved ticket attachments and their untrusted original display names;
- build output logs, portable exports, database backups, and a per-process API
  token; and
- credentials configured by the operator. Credential-shaped settings are
  protected at rest with Windows DPAPI for the current Windows profile.

The SQLite database and attachment folders are private operator data. Do not
publish them in a GitHub issue or release package. Portable exports omit secret
settings, but they can still contain private project, contributor, Discord,
knowledge, report, or evidence text and must be reviewed before sharing.

## Discord behavior

Discord integration is optional. When configured, DevHub connects using the
operator-provided bot account and processes messages in explicitly monitored
channels. Pending suggestions retain the metadata required for review. Original
attachment bytes are fetched only when an authorized workflow promotes or
captures them; validated files are then stored locally. Deleting or dismissing
records follows the documented lifecycle and may preserve scrubbed audit
markers needed to prevent replay.

Discord and GitHub have their own privacy policies. XA DevHub does not control
data those services retain independently.

## Local automation API

The automation API binds to loopback and rotates a random per-process token in
the selected data directory. The token is required for API requests and should
never be copied into source, logs, issues, or release packages. Changing the
port does not make the API appropriate for public network exposure.

## Backups, retention, and deletion

DevHub can create coherent database backups and portable exports. Database-only
backups do not contain the sibling attachment bytes, so back up the attachment
folders separately when they matter. Operators control retention and are
responsible for protecting every copy.

To remove local DevHub data, close the application and delete the chosen data
directory after preserving any required backup. Uninstalling or deleting the
portable application files does not intentionally upload or remotely delete
data.

## Sharing diagnostics

Before sharing a screenshot, log excerpt, export, database, or reproduction:

1. remove credentials, API tokens, Discord IDs, names, private project text,
   absolute profile paths, and attachment contents;
2. reproduce with a fresh disposable data directory when possible; and
3. use the private security-reporting route in `SECURITY.md` for a
   vulnerability.

This document describes XA DevHub v1.0.5. Material privacy behavior changes are
recorded in `CHANGELOG.md`.
