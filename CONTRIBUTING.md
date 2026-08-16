# Contributing to XA DevHub

Thank you for improving DevHub. Issues, forks, and pull requests are welcome.

## Before opening an issue

- Search existing issues first.
- Remove tokens, database files, Discord IDs, private project text, usernames,
  absolute profile paths, and other personal data from logs or screenshots.
- For a vulnerability, use the private process in [SECURITY.md](SECURITY.md)
  instead of a public issue.

## Development setup

DevHub currently targets 64-bit Windows and requires Visual Studio 2022 with
the Desktop development with C++ workload, CMake 3.20 or newer, Git, Python 3,
and vcpkg. Dependencies are pinned by `vcpkg.json` and
`vcpkg-configuration.json`.

Create an isolated development build rather than pointing tests at an
installation that contains real data:

```powershell
cmake -S . -B build-dev -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DDEVHUB_CONSOLE=ON
cmake --build build-dev --config Debug --parallel 4
build-dev\Debug\devhub.exe --selftest
```

The public source distribution does not include the maintainers' local
regression workspace. The built-in disposable selftest remains available in
every public build.

The numbered `1. build.py` workflow is intended for an operator's production
checkout: it can stop and restart that checkout's exact DevHub process after
backing up its database. Read its `--help` output before using it.

## Pull requests

1. Fork the repository and create a focused branch in your fork.
2. Include focused reproduction and validation steps for behavior changes.
3. Keep generated output, credentials, runtime data, and machine-local files
   out of the commit.
4. Run the built-in selftest and the public-tree audit:

   ```powershell
   build-dev\Debug\devhub.exe --selftest
   powershell.exe -NoProfile -ExecutionPolicy Bypass `
       -File tools/audit_public_repo.ps1
   ```

5. Run the native build and built-in selftest above when your change touches
   C++, CMake, vcpkg, resources, migrations, or packaging.
6. Explain behavior, tests, privacy/security impact, migration impact, and any
   gate you could not run in the pull request.

Do not weaken an assertion to make a failure disappear. If a test expectation
is obsolete, explain the intentional behavior change and replace it with a
regression for the new contract.

## Licensing

By submitting a contribution, you agree that it may be distributed under
`AGPL-3.0-or-later`, the license in [LICENSE](LICENSE). Only submit work you
have the right to license. Identify copied or adapted third-party material and
preserve all required notices.
