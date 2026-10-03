#!/usr/bin/env python3
"""Build only the x86 plugin against the existing GWToolbox SDK and install it."""

import argparse
import datetime
import os
from pathlib import Path
import pwd
import shutil
import struct
import subprocess
import tempfile


def run_build(command, root):
    log_dir = root / "build"
    log_dir.mkdir(exist_ok=True)
    with (log_dir / "build.log").open("w") as log:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        for line in process.stdout:
            log.write(line)
            if not line.startswith("Note: including file:"):
                print(line, end="", flush=True)
        if process.wait():
            raise subprocess.CalledProcessError(process.returncode, command)


def main():
    root = Path(__file__).resolve().parents[1]
    owner_uid = int(os.environ.get("SUDO_UID", os.getuid()))
    owner_gid = int(os.environ.get("SUDO_GID", os.getgid()))
    user_home = Path(pwd.getpwuid(owner_uid).pw_dir)
    parser = argparse.ArgumentParser(description=__doc__)
    fixed_toolbox = user_home / "Documents/gwtb-sep30-fix"
    default_toolbox = fixed_toolbox if (fixed_toolbox / "build-wine/GWToolboxdll/GWToolboxdll.lib").is_file() else user_home / "Documents/gwtb"
    parser.add_argument("--toolbox-root", type=Path, default=default_toolbox)
    parser.add_argument("--destination", type=Path, default=Path("/run/media/tulio/ssd/Games/Guild Wars/GWToolboxpp-config/TULIO/plugins"))
    parser.add_argument("--image", default="gwtoolboxpp-wine-msvc")
    parser.add_argument("--docker", action="store_true", help="Use the Docker toolchain instead of local MSVC/Wine")
    parser.add_argument("--toolchain-root", type=Path, default=root / "build/toolchain/msvc")
    parser.add_argument("--config", choices=("RelWithDebInfo", "Release"), default="RelWithDebInfo")
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    toolbox = args.toolbox_root.resolve()
    sdk = toolbox / "build-wine/GWToolboxdll/GWToolboxdll.lib"
    if not sdk.is_file():
        parser.error(f"Prebuilt GWToolbox SDK library not found: {sdk}. The host is never built by this script.")
    if not args.build_only and not args.destination.is_dir():
        parser.error(f"Plugin folder is missing (is the game drive mounted?): {args.destination}")
    print(f"Building WorldCompletion ({args.config}, Windows x86)...", flush=True)
    started = datetime.datetime.now().timestamp()
    if args.docker:
        if not shutil.which("docker"):
            parser.error("Docker is not installed.")
        subprocess.run(["docker", "image", "inspect", args.image], check=True, stdout=subprocess.DEVNULL)
        run_build([
            "docker", "run", "--rm", "--mount", f"type=bind,source={toolbox},target=/src,readonly",
            "--mount", f"type=bind,source={root},target=/work",
            "-w", "/work", "-e", f"CONFIG={args.config}",
            "-e", f"HOST_UID={owner_uid}", "-e", f"HOST_GID={owner_gid}", args.image,
            "sh", "/work/tools/build-in-container.sh",
        ], root)
    else:
        toolchain = args.toolchain_root.resolve()
        if not (toolchain / "bin/x86/cl").is_file():
            parser.error(f"MSVC toolchain not found: {toolchain}. See README.md for one-time setup, or use --docker.")
        run_build(["sh", str(root / "tools/build-local.sh"), str(root), str(toolbox), str(toolchain), args.config], root)
    artifact = root / "bin/WorldCompletion.dll"
    if not artifact.is_file():
        parser.error(f"Build succeeded but the DLL is missing: {artifact}")
    data = artifact.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        parser.error("Output is not a Windows DLL.")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 6 > len(data) or data[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", data, pe + 4)[0] != 0x14C:
        parser.error("Output is not the required 32-bit x86 Windows DLL.")
    if artifact.stat().st_mtime < started:
        print("Build was already up to date.", flush=True)
    output = root / "bin"
    output.mkdir(exist_ok=True)
    local = output / artifact.name
    if artifact != local:
        shutil.copy2(artifact, local)
    print(f"Built: {local}", flush=True)
    if args.build_only:
        return
    destination = args.destination / artifact.name
    if destination.exists():
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        backup = destination.with_name(f"{destination.name}.{stamp}.bak")
        shutil.copy2(destination, backup)
        if os.geteuid() == 0:
            os.chown(backup, owner_uid, owner_gid)
        print(f"Previous DLL backed up: {backup}", flush=True)
    fd, temporary = tempfile.mkstemp(prefix=".WorldCompletion-", suffix=".tmp", dir=args.destination)
    os.close(fd)
    try:
        shutil.copy2(local, temporary)
        if os.geteuid() == 0:
            os.chown(temporary, owner_uid, owner_gid)
        os.replace(temporary, destination)
    finally:
        Path(temporary).unlink(missing_ok=True)
    print(f"Installed: {destination}\nReload the plugin or restart GWToolbox to use the new DLL.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Build/install failed: {error}") from error
