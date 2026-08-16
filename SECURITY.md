# Security policy

## Supported versions

Security fixes are made on the current `main` branch and the most recent
published release. Older builds may not receive fixes.

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Use the repository's
GitHub **Security > Report a vulnerability** form to send a private security
advisory to the maintainers. Include affected versions, impact, reproduction
steps, and a minimal proof of concept with all credentials and personal data
removed.

Please do not test against systems, Discord servers, accounts, repositories,
or data you do not own or have explicit permission to use. Do not include a
real `devhub.db`, WAL/SHM files, bot token, API token, attachment, export, log,
or private project record in a report.

The maintainers will acknowledge the report, investigate it, and coordinate a
fix and disclosure as capacity permits. Please allow a reasonable private
remediation window before public disclosure.

## Security boundaries

- DevHub's HTTP API is intended for loopback use only and requires the
  per-process token stored under the selected data directory.
- Discord credentials and other credential-shaped settings are local runtime
  data and must never be committed.
- Portable exports intentionally omit secret settings, but user-authored work,
  knowledge, Discord, build, and evidence text can still be sensitive. Review
  every export manually before sharing it.
