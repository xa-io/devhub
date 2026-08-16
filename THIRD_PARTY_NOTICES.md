# Third-party notices

XA DevHub is licensed under `AGPL-3.0-or-later`. It depends on the following
separately licensed components. Versions below are the dependency set resolved
from the pinned vcpkg baseline during the 2026-08-12 audit; a later dependency
update must refresh this table and the corresponding notice files.

| Component | Resolved version | License | Relationship |
| --- | ---: | --- | --- |
| Crow | 1.3.3 | BSD-3-Clause | Direct |
| nlohmann/json | 3.12.0 | MIT | Direct |
| SQLiteCpp | 3.3.3 | MIT | Direct |
| D++ | 10.1.5 | Apache-2.0 | Direct |
| Dear ImGui | 1.92.8 | MIT | Direct |
| Asio | 1.32.0 | BSL-1.0 | Transitive through Crow/D++ |
| SQLite | 3.53.3 | Public Domain | Transitive through SQLiteCpp |
| OpenSSL | 3.6.3 | Apache-2.0 | Transitive through D++ |
| Opus | 1.5.2 | BSD-3-Clause plus published patent grants | Transitive through D++ |
| zlib | 1.3.2 | Zlib | Transitive through D++ |

The exact license texts captured from vcpkg are under
`third_party/licenses/`. Redistributors of compiled binaries must include the
applicable notices and license texts with their distribution. This file is an
inventory, not legal advice; inspect the resolved dependency graph again when
changing `vcpkg.json`, its baseline, or overlays.

## Adapted documentation

DevHub documents knowledge-processing ideas adapted from Eugeniu Ghelbur's
MIT-licensed `obsidian-second-brain` project; no dependency on that software is
bundled. Its copyright and permission notice are preserved in
`third_party/licenses/obsidian-second-brain.txt`.
