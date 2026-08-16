# Changelog

Public-facing XA DevHub changes are recorded here from public repository
preparation forward. Pre-public operator history is intentionally excluded.

## Unreleased

### Repository preparation

- Added authenticated exact project configuration reads and full-field atomic
  project creation/update so local automation can safely populate every native
  editor option and verify each saved record without direct database access.
- Added an authenticated, additive-only historical ticket import that preserves
  explicit work IDs, states and timestamps, attribution, completion events,
  inert Discord lineage and failure acknowledgements, and hash-validated saved
  attachments without updating or deleting existing destination tickets.
- Added the AGPL-3.0-or-later license, dependency notices, contribution and
  security guidance.
- Added an explicit public-file allow-list and a static audit for private data,
  credentials, machine-local paths, runtime data, generated files, and backups.
- Made fresh installations start with neutral, editable project data and no
  source-owned administrator identity or machine-specific configuration.
- Fixed the cross-project knowledge self-test to own a dedicated retained
  fixture instead of depending on temporary projects from another test section.
- Verified the repair in Release v1.0.5 with all 20 CTest targets, the explicit
  all-domain self-test, a disposable schema-v14 migration check, and restored
  authenticated runtime health.
- Added a user-run, non-publishing release preparation script that invokes the
  guarded build by default and produces one explicit portable Windows x64 ZIP,
  payload/source provenance, and SHA-256 checksums while rejecting runtime data,
  credentials, symbols, source, and unexpected files.
- Added end-user portable install/update/uninstall guidance and a privacy policy
  for local project, Discord, attachment, credential, API-token, backup, and
  export data.
- Kept `.github`, `docs`, and `tests` as ignored maintainer-only workspaces;
  public CMake checkouts build the application without those folders while
  retaining the built-in disposable `devhub.exe --selftest`.

No public release has been published yet.
