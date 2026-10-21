"""Run the native receiver's portable pipeline and client tests without hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FLAGS = ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Wconversion", "-Werror",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]


@pytest.fixture(scope="module")
def native_compiler(tmp_path_factory):
    compiler = os.environ.get("HOST_CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler or not shutil.which(compiler):
        pytest.skip("a host C++ compiler is required")
    directory = tmp_path_factory.mktemp("native-toolchain")
    source, binary = directory / "probe.cpp", directory / "probe"
    source.write_text("int main() { return 0; }\n")
    probe = subprocess.run([compiler, *FLAGS, str(source), "-o", str(binary)],
                           capture_output=True, text=True, timeout=30)
    if probe.returncode:
        pytest.skip("host compiler lacks the required C++17/ASAN/UBSAN support: " + probe.stderr)
    probe = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    if probe.returncode:
        pytest.skip("host ASAN/UBSAN runtime is unavailable: " + probe.stderr)
    return compiler


@pytest.mark.parametrize("suite", ["hud_button", "link_policy", "tcp_arrival", "jpeg_pipeline"])
def test_native_portable_cpp(suite, native_compiler, tmp_path):
    binary = tmp_path / suite
    built = subprocess.run([
        native_compiler, *FLAGS, "-I" + str(ROOT / "native"),
        "-I" + str(ROOT / "esp/components/protocol/include"),
        str(ROOT / "native/tests" / ("test_" + suite + ".cpp")), "-o", str(binary),
    ], capture_output=True, text=True, timeout=60)
    assert built.returncode == 0, built.stdout + built.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, run.stdout + run.stderr


def test_native_stream_client_profiles(native_compiler, tmp_path):
    env = dict(os.environ, HOST_CXX=native_compiler, TMPDIR=str(tmp_path))
    run = subprocess.run([sys.executable, str(ROOT / "native/tests/test_stream_client.py")],
                         capture_output=True, text=True, env=env, timeout=180)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "PSRAM input profile 0: passed" in run.stdout
    assert "PSRAM input profile 1: passed" in run.stdout
