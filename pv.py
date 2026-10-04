#!/usr/bin/env python3
"""
pv -- command-line tool for PlainVulkan (.pv) projects.

Usage:
    pv new <name>          Create a new project folder
    pv build <name>        Parse, generate C++, and compile a native binary
    pv run <name>          Build (if needed) and run the binary
    pv check <file.pv>     Parse a single file and report syntax errors
    pv edit [scene]        Launch the scene editor
    pv doctor              Check that the native build dependencies are present
    pv version              Print the PlainVulkan toolchain version

This script is deliberately the *only* thing a PlainVulkan install asks you
to put on PATH. Everything it does beyond `pv new` (pure Python, no
compilation needed) is orchestration: it makes sure the Rust pieces
(transpiler/, compiler/) are built in release mode, then shells out to
them, and finally -- for `build`/`run` -- to CMake for the actual native
C++/Vulkan compile. See README.md for the full pipeline diagram.
"""

import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

VERSION = "3.7"

# The directory this script lives in is the PlainVulkan install root --
# transpiler/, compiler/, and runtime/ are always siblings of pv.py itself,
# regardless of where the user copied `pv` on their PATH (this is exactly
# why the install instructions say "rename pv.py to pv, chmod +x it, and
# copy it onto PATH" rather than moving the whole PlainVulkan folder).
PV_HOME = Path(__file__).resolve().parent
TRANSPILER_DIR = PV_HOME / "transpiler"
COMPILER_DIR = PV_HOME / "compiler"
RUNTIME_DIR = PV_HOME / "runtime"


def usage(exit_code=2):
    print(__doc__.strip())
    sys.exit(exit_code)


def die(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def run(cmd, **kwargs):
    """Run a subprocess, streaming its output, and return the CompletedProcess."""
    print(f"$ {' '.join(str(c) for c in cmd)}")
    return subprocess.run(cmd, **kwargs)


def rustc_host():
    """Host triple rustc was built for, e.g. x86_64-pc-windows-gnu."""
    try:
        out = subprocess.check_output(["rustc", "-vV"], text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError):
        return ""
    for line in out.splitlines():
        if line.startswith("host:"):
            return line.split(":", 1)[1].strip()
    return ""


def windows_msvc_rustup_toolchain():
    """When rustc is the GNU Windows host, proc-macros still compile for
    GNU -- `--target msvc` is not enough, and windows-sys still needs
    MinGW dlltool. Switching the cargo *toolchain* to MSVC fixes host
    builds too. Returns None when no override is needed."""
    if os.name != "nt":
        return None
    host = rustc_host()
    if "windows-gnu" not in host:
        return None
    cpu = host.split("-")[0]
    return f"stable-{cpu}-pc-windows-msvc"


def cargo_cmd():
    """`cargo` argv, with an MSVC rustup toolchain override on GNU Windows."""
    cargo = ["cargo"]
    toolchain = windows_msvc_rustup_toolchain()
    if not toolchain:
        return cargo
    print(
        f"[pv] rustc host is GNU Windows, which needs MinGW dlltool.exe.\n"
        f"[pv] Using toolchain {toolchain} instead (same ABI as cl.exe)."
    )
    if not shutil.which("rustup"):
        die(
            "rustup is required to install the MSVC Rust toolchain.\n"
            "  rustup toolchain install " + toolchain + "\n"
            "  rustup default " + toolchain
        )
    add = run(["rustup", "toolchain", "install", toolchain, "--profile", "minimal"])
    if add.returncode != 0:
        die(
            f"could not install {toolchain}. From an x64 Native Tools prompt:\n"
            f"  rustup toolchain install {toolchain}\n"
            f"  rustup default {toolchain}"
        )
    return ["cargo", f"+{toolchain}"]


def _sources_newer_than(bin_path: Path) -> bool:
    """True if any Rust source or Cargo manifest in the workspace is newer
    than bin_path. Scans the compiler and transpiler crates (the compiler
    depends on the transpiler, so a change to either should trigger a
    rebuild). Best-effort: if anything can't be stat'd, err toward rebuilding.
    """
    try:
        bin_mtime = bin_path.stat().st_mtime
    except OSError:
        return True
    crate_dirs = [PV_HOME / "compiler", PV_HOME / "transpiler"]
    for root in crate_dirs:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            # Skip the target/ dir if a crate happens to have a local one.
            if "target" in path.parts:
                continue
            if path.suffix == ".rs" or path.name in ("Cargo.toml", "Cargo.lock"):
                try:
                    if path.stat().st_mtime > bin_mtime:
                        return True
                except OSError:
                    return True
    return False


def cargo_release_binary(crate_dir: Path, bin_name: str) -> Path:
    """Ensure `bin_name` is built in release mode for the crate at
    crate_dir, building it if missing, and return its path.

    The workspace root Cargo.toml means `cargo build --release` from any
    member crate's directory still places output in the shared
    <PV_HOME>/target/release/ directory (a Cargo workspace convention),
    which is where we look for the binary afterwards.
    """
    # Cargo appends .exe on Windows. Checking both means this works whether
    # pv.py is run from cmd, PowerShell, Git Bash, or WSL.
    exe_suffix = ".exe" if os.name == "nt" else ""
    bin_path = PV_HOME / "target" / "release" / (bin_name + exe_suffix)

    # Rebuild when the binary is missing OR any Rust source is newer than it.
    # Cargo is a no-op when nothing actually changed, so the extra call is
    # cheap -- and it avoids the trap where editing the compiler's .rs files
    # has no effect because a stale binary from a previous build is reused.
    if bin_path.exists() and not _sources_newer_than(bin_path):
        return bin_path

    if not shutil.which("cargo"):
        die(
            "cargo (Rust) is not installed or not on PATH.\n"
            "PlainVulkan's transpiler/compiler are written in Rust -- install Rust "
            "(e.g. via https://rustup.rs) and re-run this command."
        )

    reason = "not built yet" if not bin_path.exists() else "sources changed"
    print(f"[pv] {bin_name} {reason} -- building {crate_dir.name} in release mode...")
    result = run(cargo_cmd() + ["build", "--release"], cwd=crate_dir)
    if result.returncode != 0:
        die(f"failed to build {crate_dir.name}")
    if not bin_path.exists():
        die(f"expected binary '{bin_path}' not found after a successful build")
    return bin_path


def cmd_new(args):
    if len(args) != 1:
        print("Usage: pv new <name>")
        sys.exit(2)
    name = args[0]
    root = Path(name)
    if root.exists():
        die(f"'{root}' already exists")

    (root / "assets").mkdir(parents=True)
    (root / "scripts").mkdir(parents=True)

    main_pv = root / "main.pv"
    main_pv.write_text(STARTER_MAIN_PV.format(name=name))

    project = {
        "name": name,
        "version": "0.1.0",
        "entry": "main.pv",
        "window": {"width": 1280, "height": 720, "title": name},
        "build": {"vulkan_validation": True},
    }
    (root / "pvproject.json").write_text(json.dumps(project, indent=2) + "\n")

    print(f"Creating {name}")
    print(f"  {root}/")
    print(f"  {root}/assets/")
    print(f"  {root}/scripts/")
    print(f"  {root}/main.pv")
    print(f"  {root}/pvproject.json")
    print()
    print(f"Next steps:")
    print(f"  cd {name}")
    print(f"  pv run .")


def cmd_check(args):
    if len(args) != 1:
        print("Usage: pv check <file.pv>")
        sys.exit(2)
    pvc = cargo_release_binary(TRANSPILER_DIR, "pvc")
    result = run([str(pvc), "check", args[0]])
    sys.exit(result.returncode)


def cmd_build(args, run_after=False):
    if len(args) < 1:
        print(f"Usage: pv {'run' if run_after else 'build'} <project_dir> [-- <program args...>]")
        sys.exit(2)
    project_dir = Path(args[0]).resolve()
    if not (project_dir / "pvproject.json").exists():
        die(f"'{project_dir}' doesn't look like a PlainVulkan project (no pvproject.json)")

    extra_args = []
    if "--" in args:
        idx = args.index("--")
        extra_args = args[idx + 1 :]

    if not RUNTIME_DIR.exists():
        die(f"runtime directory not found at '{RUNTIME_DIR}' -- is this pv.py still inside the PlainVulkan folder?")

    pvcc = cargo_release_binary(COMPILER_DIR, "pvcc")
    cmd = [str(pvcc), "run" if run_after else "build", str(project_dir), "--runtime-dir", str(RUNTIME_DIR)]
    if run_after and extra_args:
        cmd += ["--"] + extra_args
    result = run(cmd)
    sys.exit(result.returncode)


def _find_editor_bin(build_dir: Path) -> Path:
    """Return the pvedit path. Multi-config generators used to write
    bin/RelWithDebInfo/pvedit.exe while this CLI looked in bin/."""
    exe_suffix = ".exe" if os.name == "nt" else ""
    name = "pvedit" + exe_suffix
    for candidate in (
        build_dir / "bin" / name,
        build_dir / "bin" / "RelWithDebInfo" / name,
        build_dir / "bin" / "Release" / name,
        build_dir / "bin" / "Debug" / name,
    ):
        if candidate.exists():
            return candidate
    return build_dir / "bin" / name


def cmd_edit(args):
    """Build (if needed) and launch the scene editor."""
    build_dir = PV_HOME / "build-editor"
    editor_bin = _find_editor_bin(build_dir)

    # Always configure once, then `cmake --build` (no-op if up to date) so
    # editing editor/ or runtime/ actually reaches the binary.
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        print("[pv] editor not configured yet -- configuring (one-time step)...")
        build_dir.mkdir(exist_ok=True)
        result = run(["cmake", "-S", str(PV_HOME / "editor-build"), "-B", str(build_dir),
                      "-DCMAKE_BUILD_TYPE=RelWithDebInfo"])
        if result.returncode != 0:
            die("failed to configure the editor build")

    print("[pv] building editor...")
    result = run(["cmake", "--build", str(build_dir), "--config", "RelWithDebInfo",
                  "-j", str(os.cpu_count() or 4)])
    if result.returncode != 0:
        die("failed to build the editor")

    editor_bin = _find_editor_bin(build_dir)
    if not editor_bin.exists():
        die(f"editor binary not found at '{editor_bin}' after building")

    result = run([str(editor_bin)] + args)
    sys.exit(result.returncode)


def cmd_doctor(_args):
    """Reports which build dependencies are present. Written because the
    single most common failure for a new contributor is a missing native
    tool, and a CMake error three layers deep is a poor way to learn that."""
    print(f"PlainVulkan {VERSION}")
    print(f"  install:  {PV_HOME}")
    print(f"  platform: {platform.system()} {platform.machine()} (python {platform.python_version()})")
    print()

    def check(name, found, hint):
        mark = "ok  " if found else "MISS"
        print(f"  [{mark}] {name}")
        if not found:
            print(f"         {hint}")

    check("cargo (Rust)", shutil.which("cargo") is not None,
          "install from https://rustup.rs -- needed to build the transpiler and compiler")
    host = rustc_host()
    if os.name == "nt" and "windows-gnu" in host and shutil.which("dlltool") is None:
        print(f"  note  rustc host is {host} (needs MinGW dlltool.exe)")
        print("        pv.py will use stable-*-pc-windows-msvc for cargo builds.")
        print("        To make that the default: rustup default stable-x86_64-pc-windows-msvc")
    check("cmake", shutil.which("cmake") is not None,
          "install CMake 3.16+ from https://cmake.org or your package manager")

    has_glsl = shutil.which("glslangValidator") or shutil.which("glslc")
    check("glslangValidator or glslc", has_glsl is not None,
          "install the Vulkan SDK (https://vulkan.lunarg.com) or your distro's glslang-tools")

    if platform.system() == "Windows":
        check("C++ compiler (MSVC)", shutil.which("cl") is not None,
              "open a 'x64 Native Tools Command Prompt for VS', or install Visual Studio "
              "Build Tools with the 'Desktop development with C++' workload")
    else:
        check("C++ compiler", (shutil.which("c++") or shutil.which("g++") or
                               shutil.which("clang++")) is not None,
              "install g++ or clang++")

    physx_root = os.environ.get("PHYSX_ROOT") or os.environ.get("PHYSX_DIR")
    if physx_root:
        print(f"  [ok  ] PhysX SDK at {physx_root}")
    else:
        print("  [note] PHYSX_ROOT not set -- the build will look in the usual system")
        print("         locations, and fall back to the builtin physics solver if it")
        print("         finds nothing. Set -DPV_FETCH_PHYSX=ON to build PhysX from source.")


def cmd_version(_args):
    print(f"PlainVulkan {VERSION}")
    print(f"  install: {PV_HOME}")


STARTER_MAIN_PV = """\
// {name} -- a new PlainVulkan project.
// Full syntax/engine-command reference: see PLAINVULKAN_REFERENCE.md in
// the PlainVulkan install root.

Pv::Init();
Pv::CreateWindow(1280, 720, "{name}");
Pv::SetAmbient(1, 1, 1, 0.3);
Pv::AddDirectionalLight(-0.4, -1, -0.3, 1, 1, 1, 0.9);

var cube = Pv::CreateCube(1.0);
var angle = 0.0;

while (Pv::IsWindowOpen()) {{
    Pv::BeginFrame();
    Pv::Clear(0x1a1a2e);

    var dt = Pv::GetDeltaTime();
    angle += dt;

    Pv::SetCamera(0, 2, -5);
    Pv::LookAt(0, 0, 0);
    Pv::DrawMeshEx(cube, 0, 0, 0, 0, angle, 0, 1.0);

    Pv::DrawText("{name}", 20, 20, 0xFFFFFF);
    Pv::DrawText(Pv::GetFPS(), 20, 40, 0xFFFFFF);

    if (Pv::IsKeyPressed(256)) {{
        Pv::CloseWindow();
    }}

    Pv::EndFrame();
}}

Pv::Shutdown();
"""


def main():
    args = sys.argv[1:]
    if not args:
        usage()

    cmd, rest = args[0], args[1:]
    if cmd == "new":
        cmd_new(rest)
    elif cmd == "build":
        cmd_build(rest, run_after=False)
    elif cmd == "run":
        cmd_build(rest, run_after=True)
    elif cmd == "check":
        cmd_check(rest)
    elif cmd == "edit":
        cmd_edit(rest)
    elif cmd == "doctor":
        cmd_doctor(rest)
    elif cmd in ("version", "--version", "-v"):
        cmd_version(rest)
    elif cmd in ("help", "--help", "-h"):
        usage(0)
    else:
        print(f"error: unknown command '{cmd}'\n", file=sys.stderr)
        usage()


if __name__ == "__main__":
    main()
