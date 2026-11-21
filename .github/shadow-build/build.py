"""Build approved public source and seal diagnostics and files for the caller."""

from __future__ import annotations

import argparse
import functools
import hashlib
import http.server
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import threading
import traceback
import urllib.request
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

SOURCES = {
    "dotnet": "haiven-sdk-dotnet",
    "unity-core": "haiven-sdk-unity",
    "godot-dotnet": "haiven-sdk-godot",
    "godot-demo": "haiven-sdk-godot-demo",
    "cpp": "haiven-sdk-cpp",
    "bossroom": "haiven-sdk-unity-bossroom",
    "unreal": "haiven-sdk-unreal",
    "lyra": "haiven-sdk-unreal-lyra",
    "unreal-chat": "haiven-sdk-unreal-chat",
}
CLI_REPOSITORY_ID = 1464751838
HOSTED = frozenset(("dotnet", "unity-core", "godot-dotnet", "godot-demo", "cpp", "cli"))
MANAGED = frozenset(("dotnet", "unity-core", "godot-dotnet"))
HOSTS = {"linux": "linux-x64", "win32": "windows-x64", "darwin": "macos"}
RUNNERS = {"linux-x64": "ubuntu-24.04", "windows-x64": "windows-2025", "macos": "macos-15"}
SHA = re.compile(r"[0-9a-f]{40}")
IDENTIFIER = re.compile(r"[1-9][0-9]{0,19}")
RECIPIENT = re.compile(r"age1[0-9a-z]{58}")
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?")
GODOT_VERSION = "4.5.1"
GODOT_ASSETS = {
    "linux-x64": (
        "linux.x86_64.zip",
        "5bccbed65a94b82c7c319fdb15719ee8113a6e503976cc54e16f1c61fe95f3d74e5e40b8449b5bb89ff7f424574c20af01a4f5ef08b389e4dc338b245185b0b9",
        "Godot_v4.5.1-stable_linux.x86_64",
    ),
    "windows-x64": (
        "win64.exe.zip",
        "ab84df90ead5a888530faaabe744a27678ef7635068883900174fae37f7fc6178d033b551d88963c7dc2466580915a582bbc79aca97c19085900655761f56a27",
        "Godot_v4.5.1-stable_win64.exe",
    ),
    "macos": (
        "macos.universal.zip",
        "a91a7c3bb2faefafef41624fbd75705b91aec509823d41db595a38b79819e0f2e9c85eecdc7429c3159d8070604a3f4b4425aaa6094d7bcac147f52c89181ce9",
        "Godot.app/Contents/MacOS/Godot",
    ),
}
TEMPLATES_SHA512 = (
    "8a65c73541184fbcf1a7afedb37c9c15ed0f3babdaafe36ff47ccc515548ac84bc7563ba5b94c253e6085b121ac608b7e03d35dfc0b2c8c690a646a8755b57e5"
)
GODOT_TARGETS = {
    "linux-x64": ("Linux", "HaivenGodotDemo.x86_64"),
    "windows-x64": ("Windows Desktop", "HaivenGodotDemo.exe"),
    "macos": ("macOS", "HaivenGodotDemo.zip"),
}


def platforms(target: str) -> tuple[str, ...]:
    return ("portable",) if target in MANAGED else tuple(RUNNERS)


def request(data: str, *, private: bool = False) -> dict[str, str]:
    raw = json.loads(data)
    if not isinstance(raw, dict) or set(raw) != {
        "target", "source_sha", "dotnet_sha", "version", "request_id", "recipient"
    } or any(not isinstance(value, str) for value in raw.values()):
        raise ValueError("invalid build request")
    target = raw["target"]
    companion = target in {"unity-core", "godot-dotnet"}
    if (
        target not in (SOURCES.keys() | {"cli"} if private else HOSTED)
        or not SHA.fullmatch(raw["source_sha"])
        or (not SHA.fullmatch(raw["dotnet_sha"]) if companion else raw["dotnet_sha"] != "")
        or not IDENTIFIER.fullmatch(raw["request_id"])
        or not RECIPIENT.fullmatch(raw["recipient"])
        or (not VERSION.fullmatch(raw["version"]) if target == "cli" else raw["version"] != "")
    ):
        raise ValueError("invalid build request")
    return raw


def public_repository(target: str) -> str:
    if target != "cli":
        return f"haiven-dev/{SOURCES[target]}"
    # Resolve the pinned canonical repository even if its public name changes.
    url = f"https://api.github.com/repositories/{CLI_REPOSITORY_ID}"
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(request, timeout=20) as response:
        repository = json.load(response)
    name = repository.get("full_name", "")
    if (
        repository.get("id") != CLI_REPOSITORY_ID
        or repository.get("visibility") != "public"
        or not re.fullmatch(r"haiven-dev/[A-Za-z0-9_.-]+", name)
    ):
        raise ValueError("CLI source repository differs from the approved public repository")
    return name


def plan(data: str) -> None:
    values = request(data)
    target = values["target"]
    matrix = {
        "include": [
            {"platform": platform, "runner": RUNNERS.get(platform, "ubuntu-24.04")}
            for platform in platforms(target)
        ]
    }
    with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
        for name in ("target", "source_sha", "dotnet_sha", "request_id"):
            print(f"{name}={values[name]}", file=output)
        print(f"repository={public_repository(target)}", file=output)
        print(f"matrix={json.dumps(matrix, separators=(',', ':'))}", file=output)


def command(*args: str | Path, cwd: Path, **environment: str) -> None:
    env = {
        name: value for name, value in os.environ.items()
        if not name.startswith(("HAIVEN_DEMO_", "HAIVEN_LYRA_"))
        and name not in {
            "GITHUB_TOKEN", "GH_TOKEN", "GIT_ASKPASS", "GITHUB_ENV", "GITHUB_OUTPUT",
            "GITHUB_PATH", "GITHUB_STATE", "GITHUB_STEP_SUMMARY",
        }
    }
    subprocess.run(
        tuple(str(arg) for arg in args),
        cwd=cwd,
        env={**env, **environment},
        check=True,
        stdout=sys.stdout,
        stderr=sys.stderr,
    )


def require_checkout(source: Path, sha: str) -> None:
    actual = subprocess.check_output(("git", "rev-parse", "HEAD"), cwd=source, text=True, stderr=sys.stderr)
    if actual.strip() != sha:
        raise ValueError("checkout does not match approved public commit")


def managed(target: str, source: Path, dotnet: Path, output: Path, work: Path) -> None:
    if target == "dotnet":
        for name in ("Haiven.Sdk", "Haiven.Sdk.SystemTextJson"):
            command("dotnet", "pack", source / name / f"{name}.csproj", "-c", "Release",
                    "-o", output, "-p:ContinuousIntegrationBuild=true", cwd=source)
            if not list(output.glob(f"{name}.[0-9]*.nupkg")):
                raise ValueError("missing portable package")
        return
    if target == "unity-core":
        project = dotnet / "Haiven.Sdk.Core/Haiven.Sdk.Core.csproj"
        assemblies = ("Haiven.Sdk.Core.dll",)
    else:
        project = source / "Haiven.Godot.csproj"
        assemblies = ("Haiven.Godot.dll", "Haiven.Sdk.Core.dll")
    compiled = work / "compiled"
    # Running below the version-pinned .NET checkout selects its global.json.
    command("dotnet", "build", project, "-c", "Release", "-o", compiled,
            "-p:ContinuousIntegrationBuild=true", cwd=dotnet)
    for name in assemblies:
        shutil.copyfile(compiled / name, output / name)


def cpp(source: Path, output: Path, work: Path) -> None:
    build, consumer = work / "cpp-build", work / "cpp-consumer"
    command("cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release", cwd=source)
    command("cmake", "--build", build, "--config", "Release", "--parallel", cwd=source)
    command("ctest", "--test-dir", build, "-C", "Release", "--output-on-failure",
            "--no-tests=error", cwd=source)
    command("cmake", "--install", build, "--config", "Release", "--prefix", output, cwd=source)
    command("cmake", "-S", source / "test/consumer", "-B", consumer,
            f"-DCMAKE_PREFIX_PATH={output}", "-DCMAKE_BUILD_TYPE=Release", cwd=source)
    command("cmake", "--build", consumer, "--config", "Release", cwd=source)
    executable = next((consumer / path for path in (
        "consumer", "consumer.exe", "Release/consumer", "Release/consumer.exe"
    ) if (consumer / path).is_file()), None)
    if executable is None:
        raise ValueError("missing compiled package consumer")
    command(executable, cwd=source)


class QuietMirror(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format: str, *args: object) -> None:
        pass


def cli(source: Path, output: Path, work: Path, version: str) -> None:
    if (source / "version.txt").read_text().strip() != version:
        raise ValueError("CLI release version differs from its approved source")
    epoch = subprocess.check_output(("git", "log", "-1", "--format=%ct"), cwd=source, text=True, stderr=sys.stderr)
    command("go", "test", "-count=1", "./tools/release", "./internal/pin", "./scripts", cwd=source)
    command("go", "run", "./tools/release", "-version", version, "-out", output, cwd=source,
            SOURCE_DATE_EPOCH=epoch.strip(), GOTOOLCHAIN="local", GOWORK="off")
    entries = (output / "checksums.txt").read_text().splitlines()
    if not entries:
        raise ValueError("CLI archives are missing")
    for entry in entries:
        checksum, name = entry.split("  ", 1)
        if not re.fullmatch(r"[A-Za-z0-9._-]+", name) or name in {".", ".."}:
            raise ValueError("invalid CLI archive name")
        with (output / name).open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != checksum:
                raise ValueError("CLI archive differs from its checksum")
    mirror = work / "mirror"
    shutil.copytree(output, mirror / f"v{version}")
    destination = work / "installed"
    handler = functools.partial(QuietMirror, directory=str(mirror))
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            download_base = f"http://127.0.0.1:{server.server_port}"
            if sys.platform == "win32":
                command("pwsh", "-NoProfile", "-File", source / "scripts/install.ps1", cwd=source,
                        HAIVEN_VERSION=version, HAIVEN_INSTALL_DIR=str(destination),
                        HAIVEN_NO_PATH="1", HAIVEN_DOWNLOAD_BASE=download_base)
                executable = destination / "haiven.exe"
            else:
                command("sh", source / "scripts/install.sh", "--version", version, "--dir",
                        destination, cwd=source, HAIVEN_DOWNLOAD_BASE=download_base)
                executable = destination / "haiven"
            receipt = subprocess.check_output((str(executable), "version", "--json"), cwd=source, stderr=sys.stderr)
            if json.loads(receipt).get("version") != version:
                raise ValueError("installed CLI version differs from the approved release")
        finally:
            server.shutdown()
            thread.join()


def godot_asset(work: Path, suffix: str, digest: str) -> Path:
    file = f"Godot_v{GODOT_VERSION}-stable_{suffix}"
    url = f"https://github.com/godotengine/godot-builds/releases/download/{GODOT_VERSION}-stable/{file}"
    destination = work / file
    with urllib.request.urlopen(url, timeout=120) as response, destination.open("xb") as stream:
        shutil.copyfileobj(response, stream)
    with destination.open("rb") as stream:
        if hashlib.file_digest(stream, "sha512").hexdigest() != digest:
            raise ValueError("Godot release asset differs from its pinned digest")
    return destination


def godot_demo(source: Path, output: Path, work: Path, platform: str) -> None:
    archive, digest, binary = GODOT_ASSETS[platform]
    editor = godot_asset(work, archive, digest)
    tools = work / "editor"
    tools.mkdir()
    if platform == "macos":
        command("ditto", "-x", "-k", editor, tools, cwd=work)
    else:
        with zipfile.ZipFile(editor) as bundle:
            bundle.extract(binary, tools)
    godot = tools / binary
    godot.chmod(0o755)
    data = {
        "linux-x64": Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "godot",
        "windows-x64": Path(os.environ.get("APPDATA", Path.home())) / "Godot",
        "macos": Path.home() / "Library/Application Support/Godot",
    }[platform]
    templates = data / "export_templates" / f"{GODOT_VERSION}.stable"
    templates.mkdir(parents=True, exist_ok=True)
    member = {
        "linux-x64": "linux_release.x86_64",
        "windows-x64": "windows_release_x86_64.exe",
        "macos": "macos.zip",
    }[platform]
    with zipfile.ZipFile(godot_asset(work, "export_templates.tpz", TEMPLATES_SHA512)) as bundle:
        for name in (member, "version.txt"):
            with bundle.open(f"templates/{name}") as item, (templates / name).open("wb") as stream:
                shutil.copyfileobj(item, stream)
    preset, launcher = GODOT_TARGETS[platform]
    executable = output / launcher
    import_log = work / "godot.log"
    for args in (("--import",), ("--export-release", preset, str(executable))):
        result = subprocess.run(
            (str(godot), "--headless", "--path", str(source), "--log-file", str(import_log), *args),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
            check=False, cwd=source, env={**os.environ, "NO_COLOR": "1"},
        )
        print(result.stdout)
        if result.returncode or re.search(r"^(?:SCRIPT ERROR|ERROR):", result.stdout, re.M):
            raise ValueError("Godot export failed")
    if platform == "macos":
        unpacked = work / "smoke"
        unpacked.mkdir()
        command("ditto", "-x", "-k", executable, unpacked, cwd=work)
        (app,) = unpacked.glob("*.app")
        command("codesign", "--verify", "--deep", "--strict", app, cwd=work)
        with (app / "Contents/Info.plist").open("rb") as stream:
            name = plistlib.load(stream)["CFBundleExecutable"]
        if not isinstance(name, str) or Path(name).name != name:
            raise ValueError("invalid Godot launcher")
        executable = app / "Contents/MacOS" / name
    elif not executable.with_suffix(".pck").is_file():
        raise ValueError("missing exported Godot data pack")
    log = work / "player.log"
    command(executable, "--headless", "--quit-after", "120", "--log-file", log, cwd=executable.parent)
    diagnostics = log.read_text(encoding="utf-8", errors="replace")
    print(diagnostics)
    if "Godot Engine v" not in diagnostics or re.search(r"^(?:SCRIPT ERROR|ERROR):", diagnostics, re.M):
        raise ValueError("Godot player launch failed")


def executable_path(variable: str) -> Path:
    result = Path(os.environ.get(variable, ""))
    if not result.is_absolute() or not result.exists():
        raise ValueError(f"{variable} must name an installed licensed build input")
    return result


def licensed(target: str, source: Path, output: Path, work: Path, platform: str) -> None:
    if target == "bossroom":
        unity_target, launcher = {
            "linux-x64": ("linux64", "HaivenBossRoom.x86_64"),
            "windows-x64": ("win64", "HaivenBossRoom.exe"),
            "macos": ("osxuniversal", "HaivenBossRoom.app"),
        }[platform]
        command(executable_path("UNITY_EDITOR"), "-batchmode", "-nographics", "-quit", "-projectPath",
                source, "-buildTarget", unity_target, "-executeMethod",
                "HaivenCrossPlatformPlayerBuild.BuildCurrent", "-logFile", "-", cwd=source)
        payload = source / "Builds/players" / platform
        if not (payload / launcher).exists():
            raise ValueError("missing built Unity player")
        shutil.copytree(payload, output, dirs_exist_ok=True)
        return
    engine = executable_path("UNREAL_ENGINE_ROOT")
    installed = json.loads((engine / "Engine/Build/Build.version").read_text(encoding="utf-8-sig"))
    version = ".".join(str(installed[key]) for key in ("MajorVersion", "MinorVersion", "PatchVersion"))
    if version != "5.8.2":
        raise ValueError("installed Unreal Engine differs from the supported version")
    uat = engine / "Engine/Build/BatchFiles" / ("RunUAT.bat" if platform == "windows-x64" else "RunUAT.sh")
    unreal_platform = {"linux-x64": "Linux", "windows-x64": "Win64", "macos": "Mac"}[platform]
    if target == "unreal":
        command(uat, "BuildPlugin", f"-Plugin={source / 'Haiven/Haiven.uplugin'}",
                f"-Package={output / 'Haiven'}", f"-TargetPlatforms={unreal_platform}",
                "-Rocket", "-Unattended", "-NoP4", cwd=source)
        suffix = {"linux-x64": ".so", "windows-x64": ".dll", "macos": ".dylib"}[platform]
        modules = output / "Haiven/Binaries" / unreal_platform
        if not any(path.stat().st_size for path in modules.glob(f"*Haiven*{suffix}")):
            raise ValueError("missing compiled plugin")
        return
    if target == "lyra":
        project = work / "Lyra"
        shutil.copytree(executable_path("HAIVEN_LYRA_CONTENT"), project)
        for relative in ("Plugins/Haiven", "Plugins/HaivenLyraDemo", "Plugins/HaivenLyraInventory",
                         "Content/HaivenTestFixtures"):
            shutil.copytree(source / relative, project / relative, dirs_exist_ok=True)
        command("git", "apply", source / "patches/0001-feat-lyra-enable-the-Haiven-plugins.patch", cwd=project)
        descriptor = project / "Lyra.uproject"
        game = "LyraGame"
    else:
        project = work / "HaivenChatPlayer"
        shutil.copytree(source, project, ignore=shutil.ignore_patterns(".git"))
        descriptor = project / "HaivenChatPlayer.uproject"
        (project / "HaivenChatStarter.uproject").rename(descriptor)
        with (project / "Config/DefaultEngine.ini").open("a", encoding="utf-8") as config:
            config.write("\n[BuildConfiguration]\nbAllowHotReloadFromIDE=False\n")
        game = "HaivenChatPlayer"
    command(uat, "BuildCookRun", f"-project={descriptor}", f"-platform={unreal_platform}",
            "-clientconfig=Shipping", "-build", "-cook", "-stage", "-pak", "-archive",
            f"-archivedirectory={output}", "-unattended", "-utf8output", "-noP4", cwd=project)
    if platform == "macos":
        launchers = [*output.rglob(f"{game}*.app/Contents/Info.plist")]
        if target == "lyra":
            launchers.extend(output.rglob("Lyra.app/Contents/Info.plist"))
    else:
        extension = "exe" if platform == "windows-x64" else "sh"
        launchers = list(output.rglob(f"{game}.{extension}"))
    if not any(path.stat().st_size for path in launchers):
        raise ValueError("missing packaged Unreal launcher")


def portable_metadata(member: tarfile.TarInfo) -> tarfile.TarInfo:
    member.uid = member.gid = 0
    member.uname = member.gname = ""
    return member


def build(args: argparse.Namespace) -> bool:
    values = request(args.request, private=args.licensed)
    target = values["target"]
    platform = args.platform
    if platform not in platforms(target) or (platform != "portable" and HOSTS[sys.platform] != platform):
        raise ValueError("incorrect build platform")
    if args.licensed != (target not in HOSTED):
        raise ValueError("incorrect runner class")
    source, dotnet = args.source.resolve(), args.dotnet.resolve()
    work = args.work.resolve()
    work.mkdir()
    output = work / "output"
    output.mkdir()
    diagnostics = work / "build.log"
    checked = False
    with diagnostics.open("w", encoding="utf-8") as log:
        with redirect_stdout(log), redirect_stderr(log):
            try:
                require_checkout(source, values["source_sha"])
                if values["dotnet_sha"]:
                    require_checkout(dotnet, values["dotnet_sha"])
                if target in MANAGED:
                    managed(target, source, dotnet, output, work)
                elif target == "cpp":
                    cpp(source, output, work)
                elif target == "cli":
                    cli(source, output, work, values["version"])
                elif target == "godot-demo":
                    godot_demo(source, output, work, platform)
                else:
                    licensed(target, source, output, work, platform)
                checked = True
            except Exception:
                traceback.print_exc()
    decision = {key: value for key, value in values.items() if key != "recipient"}
    payload = work / "payload.tar.gz"
    if checked:
        # Keep package permissions, including internal app bundle links, inside
        # an opaque archive. The collector never extracts executable outputs.
        with tarfile.open(payload, "x:gz", compresslevel=3) as binary:
            binary.add(output, arcname="output", filter=portable_metadata)
        with payload.open("rb") as stream:
            decision["payload_sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
    (work / "receipt.json").write_text(json.dumps({
        **decision, "platform": platform, "success": checked,
    }, sort_keys=True), encoding="utf-8")
    archive = work / "bundle.tar"
    with tarfile.open(archive, "x") as bundle:
        bundle.add(diagnostics, arcname="build.log", filter=portable_metadata)
        bundle.add(work / "receipt.json", arcname="receipt.json", filter=portable_metadata)
        if checked:
            bundle.add(payload, arcname=payload.name, filter=portable_metadata)
    try:
        result = subprocess.run(
            ("age", "--encrypt", "--recipient", values["recipient"], "--output",
             str(args.bundle.resolve()), str(archive)), capture_output=True, check=False,
        )
        if result.returncode:
            raise ValueError("could not seal build output")
    finally:
        shutil.rmtree(work)
    print("Shadow build passed." if checked else "Shadow build failed. Encrypted diagnostics retained.")
    return checked


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("plan")
    builder = sub.add_parser("build")
    builder.add_argument("--platform", required=True)
    builder.add_argument("--licensed", action="store_true")
    for name in ("source", "dotnet", "work", "bundle"):
        builder.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    args.request = os.environ.get("SHADOW_REQUEST", "")
    try:
        if args.command == "plan":
            plan(args.request)
            return 0
        return 0 if build(args) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError):
        print("Could not prepare or seal the shadow build.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
