#!/usr/bin/env python3
"""Test production callback registry in a small, offline Rust harness."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", required=True)
    parser.add_argument("--rustc", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--vendor", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=Path("runtime-results/socket-registry"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = root / "Bluetooth/system/gd/rust/linux"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    summary_path = output / "summary.json"
    summary_path.unlink(missing_ok=True)
    inputs = [source / "stack/src/callbacks.rs", source / "stack/src/lib.rs",
              source / "dbus_projection/dbus_macros/src/lib.rs",
              source / "dbus_projection/dbus_macros/Cargo.toml"]
    evidence = {
        "passed": False,
        "source_sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in inputs},
        "tools": {},
        "vendor": str(args.vendor.resolve()),
        "scope": "Actual callback registry tests and compilation of actual exporter macro crate; no native stack or live D-Bus dispatch.",
    }
    for name in ("cargo", "rustc", "cc"):
        binary = str(Path(getattr(args, name)).resolve())
        evidence["tools"][name] = {"path": binary, "version": subprocess.check_output(
            [binary, "--version"], text=True).splitlines()[0]}
    with tempfile.TemporaryDirectory(prefix="floss-callback-registry-") as temporary:
        work = Path(temporary)
        (work / ".cargo").mkdir()
        (work / "src").mkdir()
        (work / "macros/src").mkdir(parents=True)
        for relative in ("Cargo.toml", "src/lib.rs"):
            shutil.copyfile(source / "dbus_projection/dbus_macros" / relative, work / "macros" / relative)
        (work / "Cargo.toml").write_text('''[package]
name = "floss-callback-registry-check"
version = "0.1.0"
edition = "2021"
[workspace]
[dependencies]
tokio = { version = "=1.38.2", features = ["rt", "sync"] }
[build-dependencies]
dbus_macros = { path = "macros" }
''')
        (work / "build.rs").write_text("fn main() {}\n")
        (work / ".cargo/config.toml").write_text(
            '[source.crates-io]\nreplace-with = "cached"\n[source.cached]\ndirectory = '
            + json.dumps(str(args.vendor.resolve())) + "\n")
        stack = (source / "stack/src/lib.rs").read_text()
        begin = stack.index("pub trait RPCProxy {")
        end = stack.index("\n}", begin) + 2
        # Import the real proxy contract and registry. Stub only the surrounding
        # native stack's message channel, which these registry tests require.
        registry = str(source / "stack/src/callbacks.rs")
        (work / "src/lib.rs").write_text(stack[begin:end] + '''
pub enum Message { AdapterCallbackDisconnected(u32) }
pub struct Stack;
impl Stack {
    pub fn create_channel() -> (tokio::sync::mpsc::Sender<Message>, tokio::sync::mpsc::Receiver<Message>) {
        tokio::sync::mpsc::channel(10)
    }
}
''' + "#[path=" + json.dumps(registry) + "]\nmod callbacks;\n")
        env = os.environ.copy()
        env.update(CARGO_HOME=str(work / "cargo-home"), CARGO_NET_OFFLINE="true",
                   RUSTC=evidence["tools"]["rustc"]["path"])
        env["PATH"] = ":".join(str(Path(evidence["tools"][name]["path"]).parent)
                               for name in ("cargo", "rustc", "cc")) + ":" + env.get("PATH", "")
        env["CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER"] = evidence["tools"]["cc"]["path"]
        command = [evidence["tools"]["cargo"]["path"], "test", "--offline", "--lib"]
        result = subprocess.run(command, cwd=work, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        (output / "test.log").write_text(result.stdout)
        match = re.search(r"test result: ok\. (\d+) passed; 0 failed", result.stdout)
        evidence["test_count"] = int(match.group(1)) if match else 0
        evidence["passed"] = result.returncode == 0 and evidence["test_count"] >= 2
        evidence["exit_code"] = result.returncode
    summary_path.write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps(evidence, indent=2))
    return 0 if evidence["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
