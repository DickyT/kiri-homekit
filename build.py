#!/usr/bin/env python3
# Kiri Bridge
# CN105 HomeKit controller for Mitsubishi heat pumps
# https://kiri.dkt.moe
# https://github.com/DickyT/kiri-homekit
#
# Copyright (c) 2026
# All Rights Reserved.
# Licensed under terms of the GPL-3.0 License.

"""Project-local ESP-IDF command wrapper.

This keeps the repository clean while using the global EIM-managed ESP-IDF
installation selected for this project. It also exports flash-ready binaries
into versioned folders so flashing can reuse the latest packaged artifacts.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile
from datetime import datetime
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent
IDF_VERSION = "v5.4.1"
DEFAULT_FLASH_BAUD = 115200
SERIAL_LOG_DIR = REPO_ROOT / "serial_logs"
SERIAL_LOG_PATH = SERIAL_LOG_DIR / "latest-serial.log"
FIRMWARE_EXPORTS_DIR = REPO_ROOT / "firmware_exports"
BUILD_INFO_HEADER = REPO_ROOT / "components" / "build_info" / "include" / "build_info_generated.h"
IDF_TOOLS_PATH = Path.home() / ".espressif" / "tools"
IDF_PATH = Path.home() / ".espressif" / IDF_VERSION / "esp-idf"
IDF_PYTHON = IDF_TOOLS_PATH / "python" / IDF_VERSION / "venv" / "bin" / "python"
IDF_SCRIPT = IDF_PATH / "tools" / "idf.py"
ESPTOOL_SCRIPT = IDF_PATH / "components" / "esptool_py" / "esptool" / "esptool.py"
KIRI_PACKAGE_FORMAT = "kiri-firmware-package-v1"
KIRI_PROJECT_ID = "kiri-bridge"
KIRI_PRODUCT_NAME = "Kiri Bridge"
KIRI_WEBSITE = "https://kiri.dkt.moe"
KIRI_SOURCE = "https://github.com/DickyT/kiri-homekit"

APP_ROOTS = {
    "main": REPO_ROOT,
    "installer": REPO_ROOT / "debug_apps" / "cn105_probe",
    "cn105-probe": REPO_ROOT / "debug_apps" / "cn105_probe",
}

APP_PROJECT_NAMES = {
    "main": "kiri_bridge",
    "installer": "kiri_installer",
    "cn105-probe": "kiri_installer",
}

BOARD_PROFILES = {
    "atom-lite": {
        "id": "atom-lite",
        "name": "M5Stack ATOM Lite",
        "target": "esp32",
        "build_suffix": "",
    },
    "atoms3-lite": {
        "id": "atoms3-lite",
        "name": "M5Stack AtomS3 Lite",
        "target": "esp32s3",
        "build_suffix": "atoms3-lite",
    },
}

TOOL_PATHS = [
    IDF_TOOLS_PATH / "cmake" / "3.30.2" / "CMake.app" / "Contents" / "bin",
    IDF_TOOLS_PATH / "esp-clang" / "esp-18.1.2_20240912" / "esp-clang" / "bin",
    IDF_TOOLS_PATH / "esp-rom-elfs" / "20241011",
    IDF_TOOLS_PATH / "esp32ulp-elf" / "2.38_20240113" / "esp32ulp-elf" / "bin",
    IDF_TOOLS_PATH / "esp32ulp-elf" / "2.38_20240113" / "esp32ulp-elf" / "esp32ulp-elf" / "bin",
    IDF_TOOLS_PATH / "ninja" / "1.12.1",
    IDF_TOOLS_PATH / "openocd-esp32" / "v0.12.0-esp32-20241016" / "openocd-esp32" / "bin",
    IDF_TOOLS_PATH / "xtensa-esp-elf-gdb" / "14.2_20240403" / "xtensa-esp-elf-gdb" / "bin",
    IDF_TOOLS_PATH / "xtensa-esp-elf" / "esp-14.2.0_20241119" / "xtensa-esp-elf" / "bin",
    IDF_TOOLS_PATH / "xtensa-esp-elf" / "esp-14.2.0_20241119" / "xtensa-esp-elf" / "xtensa-esp-elf" / "bin",
    IDF_TOOLS_PATH / "python" / IDF_VERSION / "venv" / "bin",
]

SERIAL_PORT_PATTERNS = [
    "/dev/cu.usbserial*",
    "/dev/cu.wchusbserial*",
    "/dev/cu.SLAB_USBtoUART*",
    "/dev/cu.usbmodem*",
    "/dev/cu.ESP32*",
]


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def build_env() -> dict[str, str]:
    if not IDF_PATH.exists():
        fail(f"ESP-IDF {IDF_VERSION} is missing at {IDF_PATH}")
    if not IDF_PYTHON.exists():
        fail(f"ESP-IDF Python venv is missing at {IDF_PYTHON}")
    if not IDF_SCRIPT.exists():
        fail(f"idf.py is missing at {IDF_SCRIPT}")
    if not ESPTOOL_SCRIPT.exists():
        fail(f"esptool.py is missing at {ESPTOOL_SCRIPT}")

    env = os.environ.copy()
    env["ESP_IDF_VERSION"] = "5.4"
    env["IDF_TOOLS_PATH"] = str(IDF_TOOLS_PATH)
    env["IDF_COMPONENT_LOCAL_STORAGE_URL"] = f"file://{IDF_TOOLS_PATH}"
    env["IDF_PATH"] = str(IDF_PATH)
    env["ESP_ROM_ELF_DIR"] = str(IDF_TOOLS_PATH / "esp-rom-elfs" / "20241011")
    env["OPENOCD_SCRIPTS"] = str(
        IDF_TOOLS_PATH
        / "openocd-esp32"
        / "v0.12.0-esp32-20241016"
        / "openocd-esp32"
        / "share"
        / "openocd"
        / "scripts"
    )
    env["IDF_PYTHON_ENV_PATH"] = str(IDF_TOOLS_PATH / "python" / IDF_VERSION / "venv")

    path_prefix = os.pathsep.join(str(path) for path in TOOL_PATHS)
    env["PATH"] = path_prefix + os.pathsep + env.get("PATH", "")
    return env


def ensure_idf_python() -> None:
    current_python = Path(sys.executable)
    if current_python != IDF_PYTHON:
        os.execv(str(IDF_PYTHON), [str(IDF_PYTHON), str(Path(__file__).resolve()), *sys.argv[1:]])


def detect_serial_ports() -> list[str]:
    ports: list[str] = []
    for pattern in SERIAL_PORT_PATTERNS:
        ports.extend(glob.glob(pattern))
    return sorted(set(ports))


def detect_port_or_fail(port: str | None) -> str:
    if port:
        return port

    ports = detect_serial_ports()
    if len(ports) == 1:
        detected = ports[0]
        print(f"Detected ESP32 serial port: {detected}", flush=True)
        return detected
    if len(ports) > 1:
        print("Multiple ESP32-like serial ports found:", file=sys.stderr)
        for candidate in ports:
            print(f"  {candidate}", file=sys.stderr)
        print("Please rerun with --port /dev/cu.xxxxx", file=sys.stderr)
        raise SystemExit(2)

    print("No ESP32-like serial port found.", file=sys.stderr)
    print("Checked patterns:", file=sys.stderr)
    for pattern in SERIAL_PORT_PATTERNS:
        print(f"  {pattern}", file=sys.stderr)
    raise SystemExit(2)


def project_root_for(app: str) -> Path:
    root = APP_ROOTS.get(app)
    if not root:
        known = ", ".join(sorted(APP_ROOTS.keys()))
        fail(f"Unknown app '{app}'. Known apps: {known}")
    if not root.exists():
        fail(f"Project root for app '{app}' is missing at {root}")
    return root


def project_name_for(app: str) -> str:
    name = APP_PROJECT_NAMES.get(app)
    if not name:
        fail(f"Missing project name mapping for app '{app}'")
    return name


def board_profile_for(board: str) -> dict[str, str]:
    profile = BOARD_PROFILES.get(board)
    if not profile:
        known = ", ".join([*sorted(BOARD_PROFILES.keys()), "all"])
        fail(f"Unknown board '{board}'. Known boards: {known}")
    return profile


def build_version_file(project_root: Path) -> Path:
    return project_root / "build_version.cmake"


def version_string() -> str:
    return datetime.now().strftime("%Y.%m%d.%H%M%S")


def write_project_version(project_root: Path, version: str) -> None:
    build_version_file(project_root).write_text(
        "# Generated by build.py. Do not edit.\n"
        f'set(PROJECT_VER "{version}")\n',
        encoding="utf-8",
    )
    if project_root == REPO_ROOT:
        BUILD_INFO_HEADER.write_text(
            "#pragma once\n\n"
            f'#define BUILD_INFO_FIRMWARE_VERSION "{version}"\n',
            encoding="utf-8",
        )


def generate_project_version(project_root: Path) -> str:
    version = version_string()
    write_project_version(project_root, version)
    return version


def command_needs_build(args: list[str]) -> bool:
    if "--no-build" in args:
        return False
    return any(arg in {"build", "all"} for arg in args)


def run_idf(
    args: list[str],
    quiet_first: bool = False,
    project_root: Path = REPO_ROOT,
    forced_version: str | None = None,
    project_name_override: str | None = None,
    board: str = "atom-lite",
) -> int:
    profile = board_profile_for(board)
    if command_needs_build(args):
        if forced_version is not None:
            write_project_version(project_root, forced_version)
        else:
            generate_project_version(project_root)

    idf_args = list(args)
    idf_args = [
        "-B",
        str(build_dir_for(project_root, board)),
        f"-DSDKCONFIG={project_root / f'sdkconfig.{board}'}",
        f"-DIDF_TARGET={profile['target']}",
        *idf_args,
    ]
    if project_root == REPO_ROOT:
        idf_args = [f"-DKIRI_PROJECT_NAME={project_name_override or APP_PROJECT_NAMES['main']}", *idf_args]

    cmd = [str(IDF_PYTHON), str(IDF_SCRIPT), *idf_args]
    env = build_env()
    if not quiet_first:
        return subprocess.call(cmd, cwd=project_root, env=env)

    print(f"Running quietly: idf.py {' '.join(idf_args)}", flush=True)
    result = subprocess.run(
        cmd,
        cwd=project_root,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode == 0:
        print(f"Quiet run succeeded: idf.py {' '.join(idf_args)}", flush=True)
        return 0

    print(
        f"Quiet run failed with exit code {result.returncode}; re-running once with normal verbose output...",
        flush=True,
    )
    return subprocess.call(cmd, cwd=project_root, env=env)


def load_json(path: Path) -> dict:
    if not path.exists():
        fail(f"Missing required file: {path}")
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_size(value: str) -> int:
    normalized = value.strip().lower()
    if normalized.startswith("0x"):
        return int(normalized, 16)
    if normalized.endswith("mb"):
        return int(normalized[:-2]) * 1024 * 1024
    if normalized.endswith("kb"):
        return int(normalized[:-2]) * 1024
    return int(normalized, 10)


def load_partitions(project_root: Path) -> dict[str, dict[str, int | str]]:
    partitions_path = project_root / "partitions.csv"
    if not partitions_path.exists():
        fail(f"Missing partition table CSV: {partitions_path}")

    partitions: dict[str, dict[str, int | str]] = {}
    for raw_line in partitions_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 5:
            continue
        name, part_type, subtype, offset, size = fields[:5]
        partitions[name] = {
            "type": part_type,
            "subtype": subtype,
            "offset": parse_size(offset),
            "size": parse_size(size),
        }
    return partitions


def parse_flash_size(value: str | None) -> int | None:
    if not value:
        return None
    try:
        return parse_size(value)
    except ValueError:
        return None


def part_package_name(offset: str, source_relative_path: str, project_name: str) -> str:
    normalized = source_relative_path.replace("\\", "/")
    if normalized.endswith("bootloader.bin"):
        return "bootloader.bin"
    if normalized.endswith("partition-table.bin"):
        return "partition-table.bin"
    if normalized.endswith("ota_data_initial.bin"):
        return "ota_data_initial.bin"
    if normalized.endswith(f"{project_name}.bin"):
        return "app.bin"
    return Path(normalized).name


def part_logical_name(package_name: str) -> str:
    if package_name == "partition-table.bin":
        return "partition_table"
    if package_name == "ota_data_initial.bin":
        return "ota_data"
    if package_name == "app.bin":
        return "app"
    return package_name.removesuffix(".bin").replace("-", "_")


def create_kiri_package(
    version_dir: Path,
    build_dir: Path,
    project_root: Path,
    app: str,
    board: str,
    project_name: str,
    version: str,
    desc: dict,
    flash_info: dict,
) -> Path:
    board_profile = board_profile_for(board)
    partitions = load_partitions(project_root)
    flash_settings = flash_info.get("flash_settings", {})
    flash_files = flash_info.get("flash_files", {})

    package_parts: list[dict[str, object]] = []
    sha256: dict[str, str] = {}
    package_sources: dict[str, Path] = {}
    for offset, relative_path in sorted(flash_files.items(), key=lambda item: int(item[0], 16)):
        source = build_dir / relative_path
        if not source.exists():
            fail(f"Build output is missing: {source}")
        package_path = part_package_name(offset, relative_path, project_name)
        digest = sha256_file(source)
        size = source.stat().st_size
        sha256[package_path] = digest
        package_sources[package_path] = source
        package_parts.append({
            "name": part_logical_name(package_path),
            "offset": int(offset, 16),
            "path": package_path,
            "size": size,
            "sha256": digest,
        })

    app_part = next((part for part in package_parts if part["name"] == "app"), None)
    if app_part is None:
        fail("Unable to identify app.bin in flash files")

    app_offset = int(app_part["offset"])
    app_partition = next(
        (partition for partition in partitions.values()
         if partition["type"] == "app" and int(partition["offset"]) == app_offset),
        None,
    )
    if app_partition is None:
        fail(f"Unable to find OTA app partition at offset 0x{app_offset:x}")

    manifest = {
        "format": KIRI_PACKAGE_FORMAT,
        "project": KIRI_PROJECT_ID,
        "product": KIRI_PRODUCT_NAME,
        "displayName": f"{KIRI_PRODUCT_NAME} {'Production App' if app == 'main' else 'Installer'} for {board_profile['name']} ({board_profile['target']})",
        "website": KIRI_WEBSITE,
        "source": KIRI_SOURCE,
        "variant": "app" if app == "main" else "installer",
        "build_app": app,
        "board": board_profile["id"],
        "boardName": board_profile["name"],
        "boardTarget": f"{board_profile['name']} ({board_profile['target']})",
        "project_name": project_name,
        "version": version,
        "target": desc.get("target", "esp32"),
        "chipFamily": str(desc.get("target", "esp32")).upper(),
        "flashSize": parse_flash_size(flash_settings.get("flash_size")),
        "flash_settings": flash_settings,
        "extra_esptool_args": flash_info.get("extra_esptool_args", {}),
        "app": {
            "offset": app_offset,
            "maxSize": int(app_partition["size"]),
            "path": "app.bin",
            "size": int(app_part["size"]),
            "sha256": app_part["sha256"],
        },
        "parts": package_parts,
        "sha256": sha256,
        "partitions": partitions,
    }

    package_path = version_dir / f"{project_name}_{board}_{version}.kiri"
    with zipfile.ZipFile(package_path, "w", compression=zipfile.ZIP_STORED) as package:
        package.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        for package_name, source in package_sources.items():
            package.write(source, package_name)
    return package_path


def build_dir_for(project_root: Path, board: str = "atom-lite") -> Path:
    profile = board_profile_for(board)
    suffix = profile.get("build_suffix", "")
    if suffix:
        return project_root / f"build-{suffix}"
    return project_root / "build"


def export_artifacts(project_root: Path, app: str, board: str = "atom-lite") -> Path:
    build_dir = build_dir_for(project_root, board)
    desc = load_json(build_dir / "project_description.json")
    flash_info = load_json(build_dir / "flasher_args.json")
    board_profile = board_profile_for(board)

    project_name = desc.get("project_name") or project_name_for(app)
    version = desc.get("project_version") or datetime.now().strftime("%Y.%m%d.%H%M%S")
    version_dir = FIRMWARE_EXPORTS_DIR / version
    version_dir.mkdir(parents=True, exist_ok=True)

    exported_files: list[dict[str, str]] = []
    for offset, relative_path in sorted(flash_info.get("flash_files", {}).items(), key=lambda item: int(item[0], 16)):
        source = build_dir / relative_path
        if not source.exists():
            fail(f"Build output is missing: {source}")
        destination_name = f"{project_name}_{board}_{version}_{offset}.bin"
        destination = version_dir / destination_name
        shutil.copy2(source, destination)
        exported_files.append(
            {
                "offset": offset,
                "source": relative_path,
                "file_name": destination_name,
            }
        )

    kiri_package_path = create_kiri_package(
        version_dir=version_dir,
        build_dir=build_dir,
        project_root=project_root,
        app=app,
        board=board,
        project_name=project_name,
        version=version,
        desc=desc,
        flash_info=flash_info,
    )

    manifest = {
        "app": app,
        "project_name": project_name,
        "board": board_profile["id"],
        "board_name": board_profile["name"],
        "board_target": f"{board_profile['name']} ({board_profile['target']})",
        "target": board_profile["target"],
        "display_name": f"{KIRI_PRODUCT_NAME} {'Production App' if app == 'main' else 'Installer'} for {board_profile['name']} ({board_profile['target']})",
        "version": version,
        "project_root": str(project_root),
        "build_dir": str(build_dir),
        "flash_settings": flash_info.get("flash_settings", {}),
        "extra_esptool_args": flash_info.get("extra_esptool_args", {}),
        "files": exported_files,
        "kiri_package": kiri_package_path.name,
    }
    manifest_path = version_dir / f"{project_name}_{board}_{version}_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Exported firmware package: {manifest_path}", flush=True)
    print(f"Exported Kiri package: {kiri_package_path}", flush=True)
    return manifest_path


def build_and_export(
    project_root: Path,
    app: str,
    quiet_first: bool,
    board: str = "atom-lite",
    forced_version: str | None = None,
    project_name_override: str | None = None,
) -> Path | None:
    rc = run_idf(
        ["build"],
        quiet_first=quiet_first,
        project_root=project_root,
        forced_version=forced_version,
        project_name_override=project_name_override,
        board=board,
    )
    if rc != 0:
        return None
    return export_artifacts(project_root, app, board=board)


def build_all(quiet_first: bool, board: str) -> int:
    shared_version = version_string()
    manifests: list[Path] = []
    boards = list(BOARD_PROFILES.keys()) if board == "all" else [board]
    for board_name in boards:
        board_profile_for(board_name)
        for app in ("main", "installer"):
            project_root = project_root_for(app)
            manifest_path = build_and_export(
                project_root,
                app,
                quiet_first=quiet_first,
                board=board_name,
                forced_version=shared_version,
            )
            if manifest_path is None:
                return 1
            manifests.append(manifest_path)

    print(f"Build-all complete for version {shared_version} ({board}):", flush=True)
    for manifest in manifests:
        print(f"  {manifest}", flush=True)
    return 0


def latest_export_manifest(app: str, board: str = "atom-lite") -> Path:
    project_name = project_name_for(app)
    candidates = sorted(FIRMWARE_EXPORTS_DIR.glob(f"*/{project_name}_{board}_*_manifest.json"))
    if not candidates:
        fail(
            f"No exported firmware package found for app '{app}'. "
            f"Run './build.py --app {app} build' first."
        )
    return candidates[-1]


def load_manifest(path: Path) -> dict:
    manifest = load_json(path)
    manifest["_manifest_path"] = str(path)
    manifest["_version_dir"] = str(path.parent)
    return manifest


def latest_or_current_manifest(project_root: Path, app: str, board: str = "atom-lite") -> dict:
    try:
        return load_manifest(latest_export_manifest(app, board=board))
    except SystemExit:
        pass

    build_dir = build_dir_for(project_root, board)
    if not (build_dir / "flasher_args.json").exists():
        fail(
            f"No exported package and no local build found for app '{app}'. "
            f"Run './build.py --app {app} build' first."
        )
    manifest_path = export_artifacts(project_root, app, board=board)
    return load_manifest(manifest_path)


def flash_manifest(manifest: dict, port: str, baud: int) -> int:
    version_dir = Path(manifest["_version_dir"])
    flash_settings = manifest.get("flash_settings", {})
    extra = manifest.get("extra_esptool_args", {})

    parts: list[str] = []
    for entry in manifest.get("files", []):
        image_path = version_dir / entry["file_name"]
        if not image_path.exists():
            fail(f"Exported artifact is missing: {image_path}")
        parts.extend([entry["offset"], str(image_path)])

    cmd = [
        str(IDF_PYTHON),
        str(ESPTOOL_SCRIPT),
        "--chip",
        extra.get("chip", "esp32"),
        "-p",
        port,
        "-b",
        str(baud),
        "--before",
        extra.get("before", "default_reset").replace("-", "_"),
        "--after",
        extra.get("after", "hard_reset").replace("-", "_"),
        "write_flash",
        "--flash_mode",
        flash_settings.get("flash_mode", "dio"),
        "--flash_size",
        flash_settings.get("flash_size", "4MB"),
        "--flash_freq",
        flash_settings.get("flash_freq", "40m"),
        *parts,
    ]
    print(f"Flashing exported package: {Path(manifest['_manifest_path']).name}", flush=True)
    print(f"Flashing: {' '.join(cmd)}", flush=True)
    return subprocess.call(cmd, env=build_env())


def open_monitor(project_root: Path, port: str) -> int:
    return run_idf(["-p", port, "monitor"], quiet_first=False, project_root=project_root)


def flash(args: argparse.Namespace) -> int:
    port = detect_port_or_fail(args.port)
    manifest: dict
    if args.no_build:
        manifest = load_manifest(latest_export_manifest(args.app, board=args.board))
    else:
        manifest_path = build_and_export(
            args.project_root,
            args.app,
            args.quiet_first,
            board=args.board,
            project_name_override=getattr(args, "project_name_override", None),
        )
        if manifest_path is None:
            return 1
        manifest = load_manifest(manifest_path)

    rc = flash_manifest(manifest, port, args.baud)
    if rc == 0 and args.monitor:
        return open_monitor(args.project_root, port)
    return rc


def flash_esptool(args: argparse.Namespace) -> int:
    port = detect_port_or_fail(args.port)
    manifest = latest_or_current_manifest(args.project_root, args.app, board=args.board)
    return flash_manifest(manifest, port, args.baud)


def serial_log(args: argparse.Namespace) -> int:
    port = detect_port_or_fail(args.port)

    try:
        import serial
    except ImportError:
        fail("pyserial is missing in the current Python environment")

    log_file = None
    if args.save:
        SERIAL_LOG_DIR.mkdir(exist_ok=True)
        log_file = SERIAL_LOG_PATH.open("w", encoding="utf-8")
        print(f"Saving serial log copy to: {SERIAL_LOG_PATH}", flush=True)

    try:
        deadline = time.monotonic() + args.seconds
        with serial.Serial(port, args.baud, timeout=0.5) as ser:
            ser.setDTR(False)
            ser.setRTS(False)
            while time.monotonic() < deadline:
                chunk = ser.readline()
                if not chunk:
                    continue
                text = chunk.decode("utf-8", errors="replace")
                print(text, end="", flush=True)
                if log_file:
                    log_file.write(text)
                    log_file.flush()
    finally:
        if log_file:
            log_file.close()
    return 0


def main() -> int:
    argv = sys.argv[1:]

    app = "main"
    board = "atom-lite"
    project_name_override: str | None = None
    normalized_argv: list[str] = []
    skip = False
    for index, arg in enumerate(argv):
        if skip:
            skip = False
            continue
        if arg in ("--app", "-a"):
            if index + 1 >= len(argv):
                fail("Missing value after --app")
            app = argv[index + 1]
            skip = True
            continue
        if arg == "--project-name":
            if index + 1 >= len(argv):
                fail("Missing value after --project-name")
            project_name_override = argv[index + 1]
            skip = True
            continue
        if arg == "--board":
            if index + 1 >= len(argv):
                fail("Missing value after --board")
            board = argv[index + 1]
            skip = True
            continue
        normalized_argv.append(arg)
    argv = normalized_argv
    if board != "all":
        board_profile_for(board)
    project_root = project_root_for(app)

    if not (len(argv) > 0 and argv[0] in ("flash-esptool", "serial-log")):
        ensure_idf_python()

    quiet_first = False
    if "--quiet-first" in argv:
        quiet_first = True
        argv = [arg for arg in argv if arg != "--quiet-first"]
    if "-q" in argv:
        quiet_first = True
        argv = [arg for arg in argv if arg != "-q"]

    if len(argv) > 0 and argv[0] == "flash-esptool":
        parser = argparse.ArgumentParser(description="Flash the latest exported firmware package with esptool.")
        parser.add_argument("-p", "--port", help="Serial port, for example /dev/cu.usbserial-xxxx")
        parser.add_argument("-b", "--baud", default=DEFAULT_FLASH_BAUD, type=int, help="Flash baud rate")
        parsed = parser.parse_args(argv[1:])
        parsed.project_root = project_root
        parsed.app = app
        parsed.board = board
        return flash_esptool(parsed)

    if len(argv) > 0 and argv[0] == "flash-auto":
        parser = argparse.ArgumentParser(description="Build/export latest firmware package and flash it.")
        parser.add_argument("-p", "--port", help="Serial port, for example /dev/cu.usbserial-xxxx")
        parser.add_argument("-b", "--baud", default=DEFAULT_FLASH_BAUD, type=int, help="Flash baud rate")
        parser.add_argument("--monitor", action="store_true", help="Open serial monitor after flashing")
        parser.add_argument("--no-build", action="store_true", help="Skip build and flash the latest exported package")
        parser.add_argument("--quiet-first", action="store_true", help="Capture build output first, rerun verbose only on failure")
        parsed = parser.parse_args(argv[1:])
        parsed.quiet_first = parsed.quiet_first or quiet_first
        parsed.project_root = project_root
        parsed.app = app
        parsed.board = board
        parsed.project_name_override = project_name_override
        return flash(parsed)

    if len(argv) > 0 and argv[0] == "serial-log":
        parser = argparse.ArgumentParser(description="Auto-detect ESP32 serial port and print serial logs briefly.")
        parser.add_argument("-p", "--port", help="Serial port, for example /dev/cu.usbserial-xxxx")
        parser.add_argument("-b", "--baud", default=115200, type=int, help="Serial baud rate")
        parser.add_argument("-s", "--seconds", default=15, type=int, help="How many seconds to read")
        parser.add_argument("--no-save", dest="save", action="store_false", help="Do not save a local log copy")
        parser.set_defaults(save=True)
        return serial_log(parser.parse_args(argv[1:]))

    if len(argv) > 0 and argv[0] in ("buildall", "build-all"):
        return build_all(quiet_first=quiet_first, board=board)

    args = argv or ["build"]
    if board == "all":
        fail("--board all is only supported with buildall")
    rc = run_idf(args, quiet_first=quiet_first, project_root=project_root, project_name_override=project_name_override, board=board)
    if rc == 0 and command_needs_build(args):
        export_artifacts(project_root, app, board=board)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
