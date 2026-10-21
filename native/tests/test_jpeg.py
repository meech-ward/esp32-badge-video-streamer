"""Host bounds/format checks for the optional native badge JPEG decoder."""
import os
import pathlib
import shutil
import subprocess
import sys

import pytest
Image = pytest.importorskip("PIL.Image", reason="install Pillow for JPEG pixel-format tests")

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_native_jpeg_rgb565_bounds_and_recovery(tmp_path):
    decoder = pathlib.Path(os.environ.get("JPEGDEC_PATH", ROOT / "native" / "third_party" / "jpegdec"))
    if not (decoder / "JPEGDEC.cpp").is_file():
        pytest.skip("set JPEGDEC_PATH to the pinned JPEGDEC source directory")
    compiler = os.environ.get("HOST_CXX") or shutil.which("clang++") or shutil.which("g++")
    if compiler is None:
        pytest.skip("a host C++ compiler is required")
    fixtures = []
    for name, size, mode, colour, progressive in (
        ("red", (320, 240), "RGB", (255, 0, 0), False),
        ("blue", (160, 120), "RGB", (0, 0, 255), False),
        ("odd", (17, 19), "RGB", None, False),
        ("gray", (320, 240), "L", 127, False),
        ("progressive", (320, 240), "RGB", (255, 0, 0), True),
        ("colour_grid", (320, 240), "RGB", None, False),
    ):
        image = Image.new(mode, size, colour or 0)
        if colour is None:
            image.putdata([((x * 17) % 256, (y * 13) % 256, (x * y * 7) % 256)
                           for y in range(size[1]) for x in range(size[0])])
        path = tmp_path / (name + ".jpg")
        image.save(path, quality=95, progressive=progressive)
        fixtures.append(str(path))
    # Exercise both the single-pixel 4:4:4 and paired-pixel 4:2:2 DSP paths,
    # alongside the default 4:2:0 camera format and clipped MCU edges above.
    for subsampling in (0, 1):
        path = tmp_path / ("colour_grid_subsampling_" + str(subsampling) + ".jpg")
        image.save(path, quality=95, subsampling=subsampling)
        fixtures.append(str(path))
    staged = {}
    for name, options in (("dsp", ["--dsp"]), ("sram", ["--sram"]),
                          ("dsp_sram", ["--dsp", "--sram"])):
        staged[name] = tmp_path / ("jpegdec_" + name)
        prepared = subprocess.run([sys.executable, str(ROOT / "native" / "prepare_jpegdec.py"),
                                   str(decoder), str(staged[name]), *options],
                                  capture_output=True, text=True, timeout=10)
        assert prepared.returncode == 0, prepared.stdout + prepared.stderr
    hashes = []
    for name, dependency, extra in (
        ("be", decoder, []),
        ("le", decoder, ["-DNATIVE_RGB565_LE=1"]),
        ("le_dsp", staged["dsp"], ["-DNATIVE_RGB565_LE=1", "-include",
                            str(ROOT / "native" / "tests" / "jpeg_dsp_emulate.hpp")]),
        ("be_sram", staged["sram"], []),
        ("le_sram", staged["sram"], ["-DNATIVE_RGB565_LE=1"]),
        ("le_dsp_sram", staged["dsp_sram"], ["-DNATIVE_RGB565_LE=1", "-include",
                            str(ROOT / "native" / "tests" / "jpeg_dsp_emulate.hpp")]),
    ):
        binary = tmp_path / ("test_jpeg_" + name)
        command = [compiler, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-D__LINUX__", "-fsanitize=address", "-fno-omit-frame-pointer",
                   *extra, "-I" + str(ROOT / "native"), "-I" + str(dependency),
                   str(ROOT / "native" / "tests" / "test_jpeg.cpp"),
                   str(ROOT / "native" / "jpeg_decode.cpp"), str(dependency / "JPEGDEC.cpp"),
                   "-o", str(binary)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        assert built.returncode == 0, built.stdout + built.stderr
        run = subprocess.run([str(binary), *fixtures], capture_output=True, text=True, timeout=30)
        assert run.returncode == 0, run.stdout + run.stderr
        assert "NATIVE_JPEG_OK" in run.stdout
        hashes.append([line for line in run.stdout.splitlines() if line.startswith("COLOUR_HASH ")])
    # Compare every decoded pixel, normalized to RGB565 colour values. Host DSP
    # instruction emulation establishes correctness only; device speed is measured.
    assert hashes[0] and all(value == hashes[0] for value in hashes[1:])


def test_dsp_staging_does_not_modify_source_and_rejects_changed_sites(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    for name in ("JPEGDEC.cpp", "JPEGDEC.h"):
        (source / name).write_text("// upstream source\n")
    inl = "\n".join(("-1409 | (-2925 << 16)",) * 2 +
                    ("ulPixel1 |= (ulPixel2 << 16);",) +
                    ("__USAT16(ulTmp | (ulTmp2 << 16), 5)",) * 2)
    (source / "jpeg.inl").write_text(inl)
    command = [sys.executable, str(ROOT / "native" / "prepare_jpegdec.py"), str(source)]
    output = tmp_path / "staged"
    run = subprocess.run([*command, str(output), "--dsp"], capture_output=True, text=True, timeout=10)
    assert run.returncode == 0, run.stderr
    assert (source / "jpeg.inl").read_text() == inl
    assert (output / "jpeg.inl").read_text() != inl
    run = subprocess.run([*command, str(source), "--dsp"], capture_output=True, text=True, timeout=10)
    assert run.returncode != 0
    assert (source / "jpeg.inl").read_text() == inl
    (source / "jpeg.inl").write_text(inl.replace("ulPixel1 |=", "renamed |=", 1))
    run = subprocess.run([*command, str(tmp_path / "changed"), "--dsp"], capture_output=True, text=True, timeout=10)
    assert run.returncode != 0
    assert not (tmp_path / "changed" / "JPEGDEC.cpp").exists()
