############################################################################################################################
#
# Build XA DevHub's production Windows executable and prove the result is usable.
#
# This numbered wrapper applies the same ordinary build entry point used by other XA projects to DevHub's native CMake
# application. It preserves runtime data, validates the version surfaces, runs both test layers, and restores the exact
# production process only when it was running before a successful build.
#
# Core Features:
# • Creates a filtered timestamped source snapshot before building
# • Finds vcpkg through VCPKG_ROOT or the standard C:\vcpkg installation
# • Refuses custom-argument production launches before touching their process or data
# • Gracefully closes only the exact default production DevHub executable when necessary
# • Creates and integrity-checks a coherent live-database snapshot immediately after shutdown
# • Configures and builds the Visual Studio 2022 x64 Release target
# • Requires CTest and the built-in DevHub self-test on the ordinary path
# • Proves database migration against a disposable copy before opening live data
# • Requires stable version-matched localhost health after restoring the app
# • Keeps interactive success, failure, and cancellation output open for review
#
# Important Note: This script never deletes build\, because build\Release\data contains DevHub's live SQLite data.
#
# XA DevHub Build v1.11
# Production build, test, and safe process-state wrapper for XA DevHub.
# Created by: XA
# Last Updated: 2026-08-12 13:31:00
#
# ## Release Notes ##
#
# v1.11 - Exclude the ignored local operator changelog from source snapshots.
# v1.10 - Show periodic vcpkg package/configuration step progress while CMake configure is otherwise silent.
# v1.09 - Cap every native build stage at four concurrent jobs to prevent CPU and power spikes.
# v1.08 - Pause interactive windows after every outcome; add --no-pause for automation.
# v1.07 - Reject exact DevHub RUNASADMIN compatibility overrides before side effects and report elevation failures clearly.
# v1.06 - Snapshot live SQLite data immediately after shutdown and before configure/build.
# v1.05 - Refuse to manage production processes launched with custom or unreadable arguments.
# v1.04 - Add disposable migration and stable version-aware runtime health gates.
# v1.03 - Add an unconditional coherent SQLite backup before the rebuilt app can launch or migrate production data.
# v1.02 - Treat an empty exact-process query as a successful not-running result.
# v1.01 - Initial numbered build workflow for XA DevHub v1.0.1.
#
############################################################################################################################

"""XA DevHub numbered build entry point.

Usage:
    python "1. build.py"                # backup, configure, build, test, verify
    python "1. build.py" --no-backup    # skip only the source snapshot
    python "1. build.py" --no-tests     # explicit build-only path
    python "1. build.py" --no-restart   # leave a previously running app stopped
    python "1. build.py" --verbose      # request verbose CMake build output
    python "1. build.py" --no-pause     # do not wait for Enter before exiting
    python "1. build.py" --help

The normal command never removes the build tree or runtime data. If the exact
``build\\Release\\devhub.exe`` process is running, the script asks its window to
close only when its command line proves it is the normal no-argument launch.
Custom data directories, ports, headless modes, or unreadable command lines are
refused before the process is touched. The app restarts only after the new
executable passes validation. When a production database exists, a coherent
SQLite backup is always created and integrity-checked immediately after the
exact process is stopped and before configure/build begins. A disposable copy
must then pass the rebuilt executable's migration check. If the app is restored,
its exact process and version-matched health endpoint must stay healthy for a
short stability window before the build is declared successful. The wrapper
enforces a maximum of four concurrent native build jobs while preserving any
lower positive cap already requested by the operator.
During dependency compilation, the wrapper tails only the active vcpkg log and
prints periodic package, configuration, step/total, percentage, and elapsed-time
updates so a healthy build does not look stalled.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import threading
import time
import traceback
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path


# BuildRunner invokes this wrapper with anonymous pipes, where Python otherwise
# selects the Windows ANSI code page. Child output decoded with errors="replace"
# can contain U+FFFD, so make every diagnostic print safe before any output.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


PROJECT_DIR = Path(__file__).resolve().parent
BUILD_DIR = PROJECT_DIR / "build"
RELEASE_DIR = BUILD_DIR / "Release"
EXE_PATH = RELEASE_DIR / "devhub.exe"
VERSION_HEADER = PROJECT_DIR / "src" / "app" / "Version.h"
CMAKE_FILE = PROJECT_DIR / "CMakeLists.txt"
BACKUP_ROOT = PROJECT_DIR / "backups"
DATABASE_PATH = RELEASE_DIR / "data" / "devhub.db"
DATABASE_BACKUP_ROOT = RELEASE_DIR / "data" / "backups"
API_TOKEN_PATH = RELEASE_DIR / "data" / "api-token"
CMAKE_GENERATOR = "Visual Studio 17 2022"
HEALTH_URL = "http://127.0.0.1:21100/api/health"
HEALTH_STARTUP_TIMEOUT_SECONDS = 20.0
HEALTH_STABILITY_SECONDS = 3.0
BUILD_CONCURRENCY_LIMIT = 4
BUILD_CONCURRENCY_ENVIRONMENT_VARIABLES = (
    "VCPKG_MAX_CONCURRENCY",
    "CMAKE_BUILD_PARALLEL_LEVEL",
    "CL_MPCount",
)
VCPKG_PROGRESS_POLL_SECONDS = 2.0
VCPKG_PROGRESS_MIN_PRINT_SECONDS = 5.0
VCPKG_PROGRESS_HEARTBEAT_SECONDS = 10.0
VCPKG_PROGRESS_TAIL_BYTES = 256 * 1024
APPCOMPAT_LAYERS_KEY = r"Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"

BACKUP_EXCLUDE_PARTS = {
    ".git",
    ".claude-octopus",
    "backups",
    "__pycache__",
    "vcpkg_installed",
}
BACKUP_EXCLUDE_ROOT_FILES = {
    "changelog.txt",
}
BACKUP_EXCLUDE_SUFFIXES = {
    ".db",
    ".dmp",
    ".ilk",
    ".lib",
    ".log",
    ".obj",
    ".pdb",
    ".pyc",
    ".zip",
}


class BuildFailure(RuntimeError):
    """Expected build failure with a concise operator-facing message."""


def format_process_launch_error(
    label: str,
    executable: str | Path,
    exc: OSError,
) -> str:
    """Return one consistent, actionable message for process-launch failures."""
    target = str(executable)
    if getattr(exc, "winerror", None) == 740:
        try:
            is_devhub = Path(target).resolve() == EXE_PATH.resolve()
        except OSError:
            is_devhub = False
        if is_devhub:
            return (
                f"{label} could not start because Windows requires elevation for "
                f"{EXE_PATH.resolve()} (WinError 740). Clear only that executable's "
                "'Run this program as an administrator' compatibility setting "
                f"(RUNASADMIN under HKCU/HKLM\\{APPCOMPAT_LAYERS_KEY}), then rerun "
                "this build from a normal unelevated terminal."
            )
        return (
            f"{label} could not start because Windows requires elevation for {target} "
            "(WinError 740). Remove that executable's RUNASADMIN compatibility "
            "setting or correct its manifest instead of elevating the whole build."
        )
    return f"{label} could not start: {exc}"


def forced_devhub_elevation_entries(exe_path: Path) -> list[str]:
    """Read exact HKCU/HKLM AppCompat values that force DevHub to elevate."""
    import winreg

    target = str(exe_path.resolve())
    locations = [
        ("HKCU", winreg.HKEY_CURRENT_USER, 0),
        ("HKLM (64-bit view)", winreg.HKEY_LOCAL_MACHINE, winreg.KEY_WOW64_64KEY),
        ("HKLM (32-bit view)", winreg.HKEY_LOCAL_MACHINE, winreg.KEY_WOW64_32KEY),
    ]
    forced: list[str] = []
    for label, root, view in locations:
        try:
            with winreg.OpenKey(
                root,
                APPCOMPAT_LAYERS_KEY,
                0,
                winreg.KEY_READ | view,
            ) as key:
                try:
                    value, _ = winreg.QueryValueEx(key, target)
                except FileNotFoundError:
                    continue
        except FileNotFoundError:
            continue
        except OSError as exc:
            raise BuildFailure(
                f"Could not inspect {label}\\{APPCOMPAT_LAYERS_KEY} for the "
                f"exact production DevHub executable: {exc}"
            ) from exc

        value_text = value if isinstance(value, str) else str(value)
        tokens = {token.upper() for token in re.findall(r"[A-Za-z0-9_]+", value_text)}
        if "RUNASADMIN" in tokens:
            forced.append(
                f"{label}\\{APPCOMPAT_LAYERS_KEY} :: {target} = {value_text}"
            )
    return forced


def reject_forced_devhub_elevation(exe_path: Path) -> None:
    forced = forced_devhub_elevation_entries(exe_path)
    if not forced:
        return
    detail = "\n".join(f"  {entry}" for entry in forced)
    raise BuildFailure(
        "Windows compatibility settings force the exact production DevHub "
        f"executable to run as administrator:\n{detail}\n"
        "Clear only that executable's 'Run this program as an administrator' "
        "compatibility setting, then rerun this build from a normal unelevated "
        "terminal. The build wrapper did not change the registry."
    )


def banner(title: str) -> None:
    line = "=" * 80
    print(f"\n{line}\n{title}\n{line}")


def section(title: str) -> None:
    print(f"\n[{title}]")


def human_size(size: int) -> str:
    value = float(size)
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1024.0 or unit == "GB":
            return f"{value:.1f} {unit}"
        value /= 1024.0
    return f"{size} B"


def command_text(command: list[str]) -> str:
    return subprocess.list2cmdline(command)


def enforce_build_concurrency_limit() -> int:
    """Apply one inherited hard ceiling to every native build layer."""
    effective = BUILD_CONCURRENCY_LIMIT
    for variable in BUILD_CONCURRENCY_ENVIRONMENT_VARIABLES:
        raw = os.environ.get(variable, "").strip()
        if not raw:
            continue
        try:
            requested = int(raw)
        except ValueError as exc:
            raise BuildFailure(
                f"{variable} must be a positive integer, not {raw!r}."
            ) from exc
        if requested < 1:
            raise BuildFailure(f"{variable} must be at least 1, not {requested}.")
        effective = min(effective, requested)

    normalized = str(effective)
    for variable in BUILD_CONCURRENCY_ENVIRONMENT_VARIABLES:
        os.environ[variable] = normalized
    return effective


class VcpkgProgressMonitor:
    """Report progress hidden inside vcpkg's per-port Ninja logs."""

    _step_pattern = re.compile(r"^\[(\d+)/(\d+)\]", re.MULTILINE)

    def __init__(self, buildtrees_dir: Path) -> None:
        self.buildtrees_dir = buildtrees_dir
        self.started_at = time.monotonic()
        self.baseline = self._snapshot()
        self.last_status: tuple[str, str, int, int] | None = None
        self.last_print_at: float | None = None

    def _logs(self) -> list[Path]:
        if not self.buildtrees_dir.is_dir():
            return []
        return list(
            self.buildtrees_dir.glob("*/install-x64-windows-*-out.log")
        )

    @staticmethod
    def _fingerprint(path: Path) -> tuple[int, int] | None:
        try:
            stat = path.stat()
        except OSError:
            return None
        return stat.st_mtime_ns, stat.st_size

    def _snapshot(self) -> dict[Path, tuple[int, int]]:
        snapshot: dict[Path, tuple[int, int]] = {}
        for path in self._logs():
            fingerprint = self._fingerprint(path)
            if fingerprint is not None:
                snapshot[path] = fingerprint
        return snapshot

    @classmethod
    def _read_step(cls, path: Path) -> tuple[int, int] | None:
        try:
            with path.open("rb") as handle:
                handle.seek(0, os.SEEK_END)
                size = handle.tell()
                start = max(0, size - VCPKG_PROGRESS_TAIL_BYTES)
                handle.seek(start)
                text = handle.read().decode("utf-8", errors="replace")
        except OSError:
            return None
        if start:
            _, _, text = text.partition("\n")
        matches = cls._step_pattern.findall(text)
        if not matches:
            return None
        current, total = matches[-1]
        return int(current), int(total)

    @staticmethod
    def _configuration(path: Path) -> str:
        name = path.name.lower()
        if "-dbg-" in name:
            return "Debug"
        if "-rel-" in name:
            return "Release"
        return "build"

    def current_progress(self) -> tuple[str, str, int, int] | None:
        candidates: list[tuple[int, str, str, int, int]] = []
        for path in self._logs():
            fingerprint = self._fingerprint(path)
            if fingerprint is None or self.baseline.get(path) == fingerprint:
                continue
            step = self._read_step(path)
            if step is None:
                continue
            candidates.append(
                (
                    fingerprint[0],
                    path.parent.name,
                    self._configuration(path),
                    step[0],
                    step[1],
                )
            )
        if not candidates:
            return None
        _, package, configuration, current, total = max(candidates)
        return package, configuration, current, total

    @staticmethod
    def _duration(seconds: float) -> str:
        whole = max(0, int(seconds))
        minutes, remainder = divmod(whole, 60)
        return f"{minutes}m {remainder:02d}s" if minutes else f"{remainder}s"

    def report(self) -> None:
        status = self.current_progress()
        if status is None:
            return
        now = time.monotonic()
        changed = status != self.last_status
        if self.last_print_at is not None:
            since_print = now - self.last_print_at
            if changed and since_print < VCPKG_PROGRESS_MIN_PRINT_SECONDS:
                return
            if not changed and since_print < VCPKG_PROGRESS_HEARTBEAT_SECONDS:
                return
        package, configuration, current, total = status
        percent = (100.0 * current / total) if total else 0.0
        elapsed = self._duration(now - self.started_at)
        print(
            f"  [vcpkg progress] {package} {configuration}: "
            f"{current}/{total} ({percent:.0f}%) | elapsed {elapsed}",
            flush=True,
        )
        self.last_status = status
        self.last_print_at = now

    def run(self, stop: threading.Event) -> None:
        try:
            while not stop.wait(VCPKG_PROGRESS_POLL_SECONDS):
                self.report()
        except Exception as exc:
            print(
                f"  [WARN] vcpkg progress display stopped: {exc}",
                flush=True,
            )


def run_command(
    command: list[str | Path],
    *,
    label: str,
    capture: bool = False,
    progress_monitor: VcpkgProgressMonitor | None = None,
) -> subprocess.CompletedProcess[str]:
    normalized = [str(part) for part in command]
    print(f"  $ {command_text(normalized)}")
    progress_stop: threading.Event | None = None
    progress_thread: threading.Thread | None = None
    if progress_monitor is not None:
        progress_stop = threading.Event()
        progress_thread = threading.Thread(
            target=progress_monitor.run,
            args=(progress_stop,),
            name="devhub-vcpkg-progress",
            daemon=True,
        )
        progress_thread.start()
    try:
        result = subprocess.run(
            normalized,
            cwd=str(PROJECT_DIR),
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
            check=False,
        )
    except OSError as exc:
        raise BuildFailure(
            format_process_launch_error(label, normalized[0], exc)
        ) from exc
    finally:
        if progress_stop is not None:
            progress_stop.set()
        if progress_thread is not None:
            progress_thread.join(timeout=VCPKG_PROGRESS_POLL_SECONDS + 1.0)
    if capture and result.stdout:
        for line in result.stdout.rstrip().splitlines():
            print(f"    {line}")
    if result.returncode != 0:
        raise BuildFailure(f"{label} failed with exit code {result.returncode}.")
    return result


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build and verify XA DevHub's production Windows Release executable."
    )
    parser.add_argument(
        "--no-backup",
        action="store_true",
        help="skip only the filtered source snapshot; the runtime database backup remains required",
    )
    parser.add_argument(
        "--no-tests",
        action="store_true",
        help="skip CTest and devhub.exe --selftest explicitly",
    )
    parser.add_argument(
        "--no-restart",
        action="store_true",
        help="do not restart DevHub if this script had to close it",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="pass --verbose to the CMake build step",
    )
    parser.add_argument(
        "--no-pause",
        action="store_true",
        help="do not wait for Enter before an interactive console exits",
    )
    return parser.parse_args()


def read_app_version() -> str:
    if not VERSION_HEADER.is_file():
        raise BuildFailure(f"Version header is missing: {VERSION_HEADER}")
    if not CMAKE_FILE.is_file():
        raise BuildFailure(f"CMake project is missing: {CMAKE_FILE}")

    header_text = VERSION_HEADER.read_text(encoding="utf-8")
    cmake_text = CMAKE_FILE.read_text(encoding="utf-8")
    header_match = re.search(r'#define\s+DEVHUB_VERSION\s+"([^"]+)"', header_text)
    cmake_match = re.search(
        r"project\s*\(\s*xa_devhub\s+VERSION\s+([0-9]+(?:\.[0-9]+)+)",
        cmake_text,
        re.IGNORECASE,
    )
    if not header_match or not cmake_match:
        raise BuildFailure("Could not read the DevHub version from Version.h and CMakeLists.txt.")

    header_version = header_match.group(1)
    cmake_version = cmake_match.group(1)
    if header_version != cmake_version:
        raise BuildFailure(
            f"Version mismatch: Version.h={header_version}, CMakeLists.txt={cmake_version}."
        )
    return header_version


def find_vcpkg_toolchain() -> Path:
    candidates: list[Path] = []
    vcpkg_root = os.environ.get("VCPKG_ROOT")
    if vcpkg_root:
        candidates.append(Path(vcpkg_root) / "scripts" / "buildsystems" / "vcpkg.cmake")
    candidates.append(Path(r"C:\vcpkg\scripts\buildsystems\vcpkg.cmake"))

    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    checked = ", ".join(str(path) for path in candidates)
    raise BuildFailure(f"Could not find the vcpkg toolchain. Checked: {checked}")


def is_backup_excluded(path: Path) -> bool:
    relative = path.relative_to(PROJECT_DIR)
    lowered_parts = tuple(part.lower() for part in relative.parts)
    if relative.as_posix().lower() in BACKUP_EXCLUDE_ROOT_FILES:
        return True
    if any(part in BACKUP_EXCLUDE_PARTS for part in lowered_parts):
        return True
    if any(part == "build" or part.startswith("build-") for part in lowered_parts):
        return True
    if path.suffix.lower() in BACKUP_EXCLUDE_SUFFIXES:
        return True
    return ".bak" in path.name.lower()


def create_source_backup() -> Path:
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    backup_dir = BACKUP_ROOT / f"DevHub source - {timestamp}"
    suffix = 2
    while backup_dir.exists():
        backup_dir = BACKUP_ROOT / f"DevHub source - {timestamp}-{suffix}"
        suffix += 1

    backup_dir.mkdir(parents=True)
    copied = 0
    for source in PROJECT_DIR.rglob("*"):
        if source.is_symlink() or not source.is_file() or is_backup_excluded(source):
            continue
        relative = source.relative_to(PROJECT_DIR)
        destination = backup_dir / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        copied += 1

    if copied == 0:
        raise BuildFailure("Source backup did not capture any files.")
    print(f"  [OK] backups/{backup_dir.name}/ ({copied} files)")
    return backup_dir


def verify_database_integrity(
    connection: sqlite3.Connection,
    *,
    label: str,
) -> None:
    rows = [str(row[0]) for row in connection.execute("PRAGMA integrity_check").fetchall()]
    if len(rows) == 1 and rows[0].lower() == "ok":
        return

    visible = rows[:5] or ["integrity_check returned no result"]
    detail = "; ".join(visible)
    if len(rows) > len(visible):
        detail += f"; plus {len(rows) - len(visible)} more issue(s)"
    raise BuildFailure(f"{label} failed SQLite integrity_check: {detail}")


def create_production_database_backup(version: str) -> Path | None:
    """Create one coherent, verified snapshot without modifying the live database."""
    if not DATABASE_PATH.exists():
        print(
            "  [SKIP] No production database exists yet: "
            f"{DATABASE_PATH.relative_to(PROJECT_DIR)}"
        )
        return None
    if not DATABASE_PATH.is_file():
        raise BuildFailure(f"Production database path is not a file: {DATABASE_PATH}")
    try:
        if DATABASE_PATH.stat().st_size == 0:
            raise BuildFailure(f"Production database is empty: {DATABASE_PATH}")
        DATABASE_BACKUP_ROOT.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise BuildFailure(
            f"Could not prepare the production database backup directory: {exc}"
        ) from exc

    safe_version = re.sub(r"[^0-9A-Za-z._-]+", "-", version).strip("-.") or "unknown"
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    stem = f"devhub-pre-launch-v{safe_version}-{timestamp}"
    backup_path = DATABASE_BACKUP_ROOT / f"{stem}.db"
    temporary_path = DATABASE_BACKUP_ROOT / f".{stem}.db.tmp"
    suffix = 2
    while backup_path.exists() or temporary_path.exists():
        backup_path = DATABASE_BACKUP_ROOT / f"{stem}-{suffix}.db"
        temporary_path = DATABASE_BACKUP_ROOT / f".{stem}-{suffix}.db.tmp"
        suffix += 1

    try:
        source = sqlite3.connect(str(DATABASE_PATH), timeout=30.0)
        destination: sqlite3.Connection | None = None
        try:
            source.execute("PRAGMA busy_timeout = 30000")
            source.execute("PRAGMA query_only = ON")
            verify_database_integrity(source, label="Production database")

            destination = sqlite3.connect(str(temporary_path), timeout=30.0)
            destination.execute("PRAGMA busy_timeout = 30000")
            source.backup(destination)
            destination.commit()
            verify_database_integrity(destination, label="Database backup")
        finally:
            if destination is not None:
                destination.close()
            source.close()

        temporary_path.replace(backup_path)
        backup_size = backup_path.stat().st_size
    except BuildFailure:
        try:
            temporary_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    except (OSError, sqlite3.Error) as exc:
        try:
            temporary_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise BuildFailure(
            "Could not create the pre-launch production database backup; the rebuilt "
            f"application will not be started. Source database was left unchanged. {exc}"
        ) from exc

    print(
        "  [OK] Coherent SQLite backup: "
        f"{backup_path.relative_to(PROJECT_DIR)} ({human_size(backup_size)})"
    )
    print("  [OK] Source and backup passed PRAGMA integrity_check.")
    return backup_path


def run_migration_probe(database_backup: Path | None, version: str) -> bool:
    """Run the rebuilt migrator against a disposable copy of the verified backup."""
    if database_backup is None:
        print("  [SKIP] No production database existed, so no migration probe is required.")
        return False

    safe_version = re.sub(r"[^0-9A-Za-z._-]+", "-", version).strip("-.") or "unknown"
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    stem = f"devhub-migration-probe-v{safe_version}-{timestamp}"
    probe_path = DATABASE_BACKUP_ROOT / f"{stem}.db"
    suffix = 2
    while probe_path.exists():
        probe_path = DATABASE_BACKUP_ROOT / f"{stem}-{suffix}.db"
        suffix += 1

    try:
        shutil.copy2(database_backup, probe_path)
    except OSError as exc:
        try:
            probe_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise BuildFailure(f"Could not create the disposable migration probe: {exc}") from exc

    command = [str(EXE_PATH), "--check-db", str(probe_path)]
    print(f"  $ {command_text(command)}")
    try:
        result = subprocess.run(
            command,
            cwd=str(PROJECT_DIR),
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    except OSError as exc:
        print(f"  [KEEP] Migration probe retained for diagnostics: {probe_path}")
        raise BuildFailure(
            f"{format_process_launch_error('Rebuilt database checker', EXE_PATH, exc)} "
            f"Probe retained at {probe_path}."
        ) from exc

    if result.stdout:
        for line in result.stdout.rstrip().splitlines():
            print(f"    {line}")
    if result.returncode != 0:
        print(f"  [KEEP] Migration probe retained for diagnostics: {probe_path}")
        raise BuildFailure(
            "Rebuilt DevHub failed its disposable production-database migration probe "
            f"with exit code {result.returncode}. Probe retained at {probe_path}"
        )

    probe_files = [
        probe_path,
        Path(f"{probe_path}-wal"),
        Path(f"{probe_path}-shm"),
        Path(f"{probe_path}-journal"),
    ]
    cleanup_errors: list[str] = []
    for path in probe_files:
        try:
            path.unlink(missing_ok=True)
        except OSError as exc:
            cleanup_errors.append(f"{path}: {exc}")
    if cleanup_errors:
        detail = "; ".join(cleanup_errors)
        raise BuildFailure(
            "Migration probe passed, but its disposable files could not be removed. "
            f"Diagnostic probe prefix: {probe_path}. {detail}"
        )

    print("  [OK] Disposable copy passed devhub.exe --check-db and was removed.")
    return True


def powershell_executable() -> str:
    executable = shutil.which("powershell.exe") or shutil.which("powershell")
    if not executable:
        raise BuildFailure("Windows PowerShell is required for exact DevHub process checks.")
    return executable


def matching_devhub_pids() -> list[int]:
    if os.name != "nt":
        return []
    environment = os.environ.copy()
    environment["DEVHUB_BUILD_EXE"] = str(EXE_PATH.resolve())
    script = r"""
$ErrorActionPreference = 'Stop'
$target = [IO.Path]::GetFullPath($env:DEVHUB_BUILD_EXE)
try {
    Get-CimInstance -ClassName Win32_Process -Filter "Name = 'devhub.exe'" |
        ForEach-Object {
        if ($_.ExecutablePath -and
            ([IO.Path]::GetFullPath($_.ExecutablePath) -ieq $target)) {
            Write-Output ([int]$_.ProcessId)
        }
    }
    exit 0
} catch {
    Write-Error $_
    exit 3
}
"""
    result = subprocess.run(
        [
            powershell_executable(),
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            script,
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=environment,
        check=False,
    )
    if result.returncode == 3:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown PowerShell error"
        raise BuildFailure(
            "Could not enumerate running DevHub processes, so the build cannot "
            "prove the production executable and database are idle. "
            f"PowerShell reported: {detail}"
        )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown PowerShell error"
        raise BuildFailure(f"Could not inspect the production DevHub process: {detail}")

    pids: list[int] = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line:
            try:
                pids.append(int(line))
            except ValueError as exc:
                raise BuildFailure(f"Unexpected process-inspection output: {line}") from exc
    return sorted(set(pids))


def matching_devhub_launches() -> list[tuple[int, str | None]]:
    """Read command lines only for processes whose executable path exactly matches DevHub."""
    if os.name != "nt":
        return []
    environment = os.environ.copy()
    environment["DEVHUB_BUILD_EXE"] = str(EXE_PATH.resolve())
    script = r"""
$ErrorActionPreference = 'Stop'
$target = [IO.Path]::GetFullPath($env:DEVHUB_BUILD_EXE)
$matches = @()
try {
    Get-CimInstance -ClassName Win32_Process -Filter "Name = 'devhub.exe'" | ForEach-Object {
        if ($_.ExecutablePath -and ([IO.Path]::GetFullPath($_.ExecutablePath) -ieq $target)) {
            $commandLine = $null
            if ($null -ne $_.CommandLine) { $commandLine = [string]$_.CommandLine }
            $matches += [pscustomobject]@{
                pid = [int]$_.ProcessId
                command_line = $commandLine
            }
        }
    }
    [pscustomobject]@{ processes = $matches } | ConvertTo-Json -Compress -Depth 3
    exit 0
} catch {
    Write-Error $_
    exit 3
}
"""
    result = subprocess.run(
        [
            powershell_executable(),
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            script,
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=environment,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown PowerShell error"
        raise BuildFailure(f"Could not inspect the production DevHub command line: {detail}")

    try:
        payload = json.loads(result.stdout)
        processes = payload["processes"]
    except (json.JSONDecodeError, KeyError, TypeError) as exc:
        raise BuildFailure("Production process command-line output was malformed.") from exc
    if not isinstance(processes, list):
        raise BuildFailure("Production process command-line output was not a list.")

    launches: list[tuple[int, str | None]] = []
    seen_pids: set[int] = set()
    for process in processes:
        if not isinstance(process, dict):
            raise BuildFailure("Production process command-line entry was malformed.")
        pid = process.get("pid")
        command_line = process.get("command_line")
        if isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0:
            raise BuildFailure("Production process command-line PID was malformed.")
        if pid in seen_pids:
            raise BuildFailure(f"Production command-line output repeated PID {pid}.")
        if command_line is not None and not isinstance(command_line, str):
            raise BuildFailure(f"Production command line for PID {pid} was malformed.")
        if command_line is not None and len(command_line) > 32_767:
            raise BuildFailure(f"Production command line for PID {pid} was oversized.")
        seen_pids.add(pid)
        launches.append((pid, command_line))
    return sorted(launches, key=lambda launch: launch[0])


def split_windows_command_line(command_line: str) -> list[str]:
    """Parse one Windows process command line using the operating system's own rules."""
    text = command_line.strip()
    if not text:
        raise ValueError("command line is empty")
    if os.name != "nt":
        raise ValueError("Windows command-line parsing is unavailable on this platform")

    argument_count = ctypes.c_int()
    command_line_to_argv = ctypes.windll.shell32.CommandLineToArgvW
    command_line_to_argv.argtypes = [
        ctypes.c_wchar_p,
        ctypes.POINTER(ctypes.c_int),
    ]
    command_line_to_argv.restype = ctypes.POINTER(ctypes.c_wchar_p)
    arguments = command_line_to_argv(text, ctypes.byref(argument_count))
    if not arguments:
        raise ValueError("CommandLineToArgvW could not parse the process command line")
    try:
        return [arguments[index] for index in range(argument_count.value)]
    finally:
        local_free = ctypes.windll.kernel32.LocalFree
        local_free.argtypes = [ctypes.c_void_p]
        local_free.restype = ctypes.c_void_p
        local_free(ctypes.cast(arguments, ctypes.c_void_p))


def require_default_devhub_launch(
    running_pids: list[int],
    launches: list[tuple[int, str | None]],
) -> None:
    """Fail closed unless every exact production process has only its argv[0]."""
    launch_by_pid = {pid: command_line for pid, command_line in launches}
    expected_pids = set(running_pids)
    observed_pids = set(launch_by_pid)
    if observed_pids != expected_pids:
        missing = sorted(expected_pids - observed_pids)
        extra = sorted(observed_pids - expected_pids)
        detail_parts: list[str] = []
        if missing:
            detail_parts.append("unreadable PID " + ", ".join(str(pid) for pid in missing))
        if extra:
            detail_parts.append("racing PID " + ", ".join(str(pid) for pid in extra))
        detail = "; ".join(detail_parts) or "process set changed during inspection"
        raise BuildFailure(
            "Could not prove the exact production DevHub command line "
            f"({detail}). No process was closed; rerun after process state is stable."
        )

    rejected: list[str] = []
    for pid in sorted(expected_pids):
        command_line = launch_by_pid[pid]
        if command_line is None:
            rejected.append(f"PID {pid}: command line unavailable")
            continue
        try:
            arguments = split_windows_command_line(command_line)
        except ValueError:
            rejected.append(f"PID {pid}: command line unreadable")
            continue
        if len(arguments) != 1 or not arguments[0].strip():
            rejected.append(f"PID {pid}: custom arguments detected")

    if rejected:
        detail = "; ".join(rejected)
        raise BuildFailure(
            "Refusing to close or restart a non-default production DevHub launch "
            f"({detail}). This wrapper protects only the default data path and port 21100; "
            "close custom --data-dir, --port, --headless, or other argument launches "
            "manually before building."
        )
    print("  [OK] Exact production command line is the default no-argument launch.")


def close_running_devhub(pids: list[int]) -> None:
    if not pids:
        print("  [OK] Production DevHub is not running.")
        return

    for pid in pids:
        print(f"  [STOP] Requesting graceful close for exact production PID {pid}...")
        environment = os.environ.copy()
        environment["DEVHUB_BUILD_PID"] = str(pid)
        script = r"""
$process = Get-Process -Id ([int]$env:DEVHUB_BUILD_PID) -ErrorAction SilentlyContinue
if ($null -eq $process) { exit 0 }
if (-not $process.CloseMainWindow()) { exit 2 }
"""
        result = subprocess.run(
            [
                powershell_executable(),
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                script,
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=environment,
            check=False,
        )
        if result.returncode != 0:
            raise BuildFailure(
                f"PID {pid} did not accept a graceful window close. Stop that exact process "
                "manually and rerun; no force-kill was attempted."
            )

    deadline = time.monotonic() + 15.0
    remaining = set(pids)
    while remaining and time.monotonic() < deadline:
        remaining.intersection_update(matching_devhub_pids())
        if remaining:
            time.sleep(0.25)
    if remaining:
        joined = ", ".join(str(pid) for pid in sorted(remaining))
        raise BuildFailure(
            f"DevHub did not finish closing within 15 seconds (PID {joined}). "
            "No force-kill was attempted."
        )
    print("  [OK] Exact production process closed.")


def configure(toolchain: Path) -> None:
    vcpkg_buildtrees = toolchain.parents[2] / "buildtrees"
    run_command(
        [
            "cmake",
            "-S",
            PROJECT_DIR,
            "-B",
            BUILD_DIR,
            "-G",
            CMAKE_GENERATOR,
            "-A",
            "x64",
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
            "-DDEVHUB_CONSOLE=OFF",
        ],
        label="CMake configure",
        progress_monitor=VcpkgProgressMonitor(vcpkg_buildtrees),
    )


def build(verbose: bool, concurrency: int) -> None:
    command: list[str | Path] = [
        "cmake",
        "--build",
        BUILD_DIR,
        "--config",
        "Release",
        "--parallel",
        str(concurrency),
    ]
    if verbose:
        command.append("--verbose")
    run_command(command, label="Release build")


def run_tests() -> None:
    run_command(
        ["ctest", "--test-dir", BUILD_DIR, "-C", "Release", "--output-on-failure"],
        label="CTest",
    )
    run_command([EXE_PATH, "--selftest"], label="DevHub self-test", capture=True)


def read_executable_versions() -> tuple[str, str]:
    environment = os.environ.copy()
    environment["DEVHUB_BUILD_EXE"] = str(EXE_PATH.resolve())
    script = r"""
$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($env:DEVHUB_BUILD_EXE)
@{ FileVersion = $version.FileVersion; ProductVersion = $version.ProductVersion } |
    ConvertTo-Json -Compress
"""
    result = subprocess.run(
        [
            powershell_executable(),
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            script,
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=environment,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown PowerShell error"
        raise BuildFailure(f"Could not read executable version metadata: {detail}")
    try:
        payload = json.loads(result.stdout)
        return str(payload["FileVersion"]), str(payload["ProductVersion"])
    except (json.JSONDecodeError, KeyError, TypeError) as exc:
        raise BuildFailure("Executable version metadata was missing or malformed.") from exc


def verify_output(expected_version: str) -> tuple[str, str]:
    if not EXE_PATH.is_file():
        raise BuildFailure(f"Expected output was not produced: {EXE_PATH}")
    file_version, product_version = read_executable_versions()
    for label, value in (("FileVersion", file_version), ("ProductVersion", product_version)):
        match = re.search(r"[0-9]+(?:\.[0-9]+){2}", value)
        if not match or match.group(0) != expected_version:
            raise BuildFailure(
                f"{label} mismatch: expected {expected_version}, executable reports {value}."
            )
    print(
        f"  [OK] {EXE_PATH.relative_to(PROJECT_DIR)} "
        f"({human_size(EXE_PATH.stat().st_size)})"
    )
    print(f"  [OK] FileVersion={file_version}; ProductVersion={product_version}")
    return file_version, product_version


def fetch_runtime_health(expected_version: str) -> tuple[bool, str]:
    headers = {"Accept": "application/json", "Connection": "close"}
    if API_TOKEN_PATH.exists():
        try:
            token = API_TOKEN_PATH.read_text(encoding="ascii").strip()
        except (OSError, UnicodeError) as exc:
            return False, f"could not read the runtime API token: {exc}"
        if re.fullmatch(r"[0-9a-f]{64}", token) is None:
            return False, "runtime api-token is not 64 lowercase hexadecimal characters"
        headers["X-DevHub-Token"] = token
    # No file is a compatibility path for pre-auth binaries only. An auth-aware
    # binary creates the file before binding, so its health check will return
    # 401 until the fresh token becomes available and the retry loop rereads it.
    request = urllib.request.Request(
        HEALTH_URL,
        headers=headers,
        method="GET",
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=1.5) as response:
            body = response.read(65_537)
    except urllib.error.HTTPError as exc:
        return False, f"HTTP {exc.code} from {HEALTH_URL}"
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        return False, f"health request failed: {exc}"

    if len(body) > 65_536:
        return False, "health response exceeded 64 KiB"
    try:
        payload = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        return False, f"health response was not valid UTF-8 JSON: {exc}"
    if not isinstance(payload, dict):
        return False, "health response was not a JSON object"
    if payload.get("ok") is not True:
        return False, f"health ok was not true: {payload.get('ok')!r}"
    if payload.get("version") != expected_version:
        return False, (
            f"health version mismatch: expected {expected_version}, "
            f"received {payload.get('version')!r}"
        )
    return True, f"ok=true, version={expected_version}"


def wait_for_runtime_health(
    process: subprocess.Popen[bytes],
    expected_version: str,
) -> None:
    deadline = time.monotonic() + HEALTH_STARTUP_TIMEOUT_SECONDS
    stable_since: float | None = None
    last_detail = "health endpoint has not responded yet"

    while time.monotonic() < deadline:
        exit_code = process.poll()
        if exit_code is not None:
            raise BuildFailure(
                f"Rebuilt DevHub exited with code {exit_code} before health became stable."
            )

        matches = matching_devhub_pids()
        if process.pid not in matches:
            stable_since = None
            last_detail = f"PID {process.pid} was not confirmed as the exact production executable"
        else:
            healthy, detail = fetch_runtime_health(expected_version)
            last_detail = detail
            now = time.monotonic()
            if healthy:
                if stable_since is None:
                    stable_since = now
                    print(
                        f"  [WAIT] Health is correct; proving {HEALTH_STABILITY_SECONDS:.1f}s "
                        "of continuous process and endpoint stability..."
                    )
                if now - stable_since >= HEALTH_STABILITY_SECONDS:
                    print(
                        f"  [OK] {HEALTH_URL} stayed healthy "
                        f"({detail}) for {HEALTH_STABILITY_SECONDS:.1f}s."
                    )
                    return
            else:
                stable_since = None
        time.sleep(0.25)

    raise BuildFailure(
        f"Rebuilt DevHub did not maintain valid health within "
        f"{HEALTH_STARTUP_TIMEOUT_SECONDS:.1f}s: {last_detail}"
    )


def restart_devhub(expected_version: str) -> int:
    creation_flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    try:
        process = subprocess.Popen(
            [str(EXE_PATH)],
            cwd=str(RELEASE_DIR),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            close_fds=True,
            creationflags=creation_flags,
        )
    except OSError as exc:
        raise BuildFailure(
            format_process_launch_error("Rebuilt DevHub executable", EXE_PATH, exc)
        ) from exc

    print(f"  [WAIT] Rebuilt DevHub started as PID {process.pid}; checking health...")
    try:
        wait_for_runtime_health(process, expected_version)
    except BuildFailure as health_error:
        close_detail = ""
        try:
            if process.poll() is None and process.pid in matching_devhub_pids():
                close_running_devhub([process.pid])
                close_detail = " The unhealthy restarted process was closed gracefully."
        except BuildFailure as close_error:
            close_detail = f" Could not close unhealthy PID {process.pid}: {close_error}"
        raise BuildFailure(f"{health_error}{close_detail}") from health_error

    print(f"  [OK] Production DevHub restart verified (PID {process.pid}).")
    return process.pid


def preflight() -> Path:
    if os.name != "nt":
        raise BuildFailure("XA DevHub's production build wrapper must run on Windows.")
    reject_forced_devhub_elevation(EXE_PATH)
    if not shutil.which("cmake"):
        raise BuildFailure("'cmake' is not on PATH.")
    if not shutil.which("ctest"):
        raise BuildFailure("'ctest' is not on PATH.")
    powershell_executable()
    return find_vcpkg_toolchain()


def main() -> int:
    args = parse_arguments()
    try:
        version = read_app_version()
        build_concurrency = enforce_build_concurrency_limit()
        toolchain = preflight()

        banner(f"XA DevHub - Build v{version}")
        print(f"  Project:   {PROJECT_DIR}")
        print(f"  Time:      {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        print(f"  Generator: {CMAKE_GENERATOR} (x64)")
        print(f"  Toolchain: {toolchain}")
        print(f"  Jobs:      {build_concurrency} maximum")

        section("1/8 Source backup")
        if args.no_backup:
            print("  [SKIP] Source backup explicitly disabled (--no-backup).")
        else:
            create_source_backup()

        section("2/8 Exact process safety")
        running_pids = matching_devhub_pids()
        if running_pids:
            require_default_devhub_launch(running_pids, matching_devhub_launches())
        close_running_devhub(running_pids)

        section("3/8 Production database backup")
        unexpected_pids = matching_devhub_pids()
        if unexpected_pids:
            joined = ", ".join(str(pid) for pid in unexpected_pids)
            raise BuildFailure(
                "The exact production DevHub executable reappeared after shutdown "
                f"(PID {joined}). Close it and rerun so the live database can be "
                "snapshotted before configure/build."
            )
        database_backup = create_production_database_backup(version)

        section("4/8 Configure")
        configure(toolchain)

        section("5/8 Build Release")
        build(args.verbose, build_concurrency)

        section("6/8 Verify and test")
        verify_output(version)
        if args.no_tests:
            print("  [SKIP] CTest and self-test explicitly disabled (--no-tests).")
        else:
            run_tests()
            print("  [OK] CTest and DevHub self-test passed.")

        section("7/8 Disposable migration probe")
        unexpected_pids = matching_devhub_pids()
        if unexpected_pids:
            joined = ", ".join(str(pid) for pid in unexpected_pids)
            raise BuildFailure(
                "The exact production DevHub executable started during the build "
                f"(PID {joined}). Close it and rerun; live data must stay stopped "
                "until the disposable migration probe passes."
            )
        migration_probed = run_migration_probe(database_backup, version)

        section("8/8 Restore prior process state")
        restarted_pid: int | None = None
        if running_pids and args.no_restart:
            print("  [SKIP] DevHub was running but will remain stopped (--no-restart).")
        elif running_pids:
            restarted_pid = restart_devhub(version)
        else:
            print("  [OK] DevHub was not running before the build; it remains stopped.")

        banner("BUILD SUCCESSFUL")
        print(f"  Output:  {EXE_PATH}")
        print(f"  Version: v{version}")
        print(f"  Tests:   {'SKIPPED by request' if args.no_tests else 'CTest + self-test passed'}")
        if database_backup is not None:
            print(f"  Data:    backed up to {database_backup}")
        else:
            print("  Data:    no production database existed")
        print(
            "  Migration: "
            + ("disposable copy passed --check-db" if migration_probed else "not applicable")
        )
        if restarted_pid is not None:
            print(
                f"  Runtime: restored as PID {restarted_pid}; {HEALTH_URL} stayed valid "
                f"for {HEALTH_STABILITY_SECONDS:.1f}s"
            )
        elif running_pids:
            print("  Runtime: left stopped by request")
        else:
            print("  Runtime: unchanged (was not running)")
        return 0
    except BuildFailure as exc:
        print(f"\n[BUILD FAILED] {exc}", file=sys.stderr)
        print(
            "The script did not force-kill a process or remove build/runtime data.",
            file=sys.stderr,
        )
        return 1


def write_crash_log(exc: Exception) -> Path | None:
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    error_message = (
        f"\n{'=' * 80}\n"
        "[CRITICAL ERROR] Build script crashed with an unhandled exception:\n"
        f"{'=' * 80}\n"
        f"Exception Type: {type(exc).__name__}\n"
        f"Exception Message: {exc}\n\n"
        f"Full Traceback:\n{traceback.format_exc()}"
        f"{'=' * 80}\n"
    )
    print(error_message, file=sys.stderr)
    crash_log_path = PROJECT_DIR / f"crash_log_{timestamp}.log"
    try:
        crash_log_path.write_text(
            f"Crash Log - {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n"
            + error_message,
            encoding="utf-8",
        )
        print(f"[CRASH LOG SAVED] {crash_log_path}", file=sys.stderr)
        return crash_log_path
    except Exception as log_error:
        print(f"[CRASH LOG] Failed to save crash log: {log_error}", file=sys.stderr)
        return None


def pause_before_exit(exit_code: int, *, no_pause: bool) -> None:
    """Keep an interactive console open without blocking redirected automation."""
    if no_pause or not sys.stdin or not sys.stdin.isatty():
        return

    if exit_code == 0:
        print("\n[BUILD COMPLETE] Review the build results above.")
    elif exit_code == 130:
        print("\n[BUILD CANCELLED] Review the output above.")
    else:
        print("\n[BUILD STOPPED] Review the error output above.")
    try:
        input("Press Enter to close this window...")
    except (EOFError, KeyboardInterrupt):
        print()


if __name__ == "__main__":
    exit_code = 1
    no_pause = "--no-pause" in sys.argv[1:]
    try:
        exit_code = main()
    except SystemExit as exit_signal:
        exit_code = int(exit_signal.code or 0)
    except KeyboardInterrupt:
        print("\n[BUILD CANCELLED] Interrupted by the operator.", file=sys.stderr)
        exit_code = 130
    except Exception as error:
        write_crash_log(error)
        print("\n[CRASH] The build script has stopped.", file=sys.stderr)
        exit_code = 1
    pause_before_exit(exit_code, no_pause=no_pause)
    raise SystemExit(exit_code)
