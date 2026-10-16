"""tools/install_tufty.py against a temp dir standing in for the badge volume. No device."""
import importlib.util
import os
import pathlib

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("install_tufty", ROOT / "tools" / "install_tufty.py")
I = importlib.util.module_from_spec(spec)
spec.loader.exec_module(I)


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


@pytest.fixture
def src(tmp_path):
    s = tmp_path / "src" / "video_streamer"
    write(s / "__init__.py", b"app")
    write(s / "net.py", b"net")
    write(s / "wifi_config.py", b"SSID = 'placeholder'\n")
    write(s / "wifi_config.example.py", b"SSID = 'not installed'\n")
    write(s / "assets" / "a.jpg", b"\xff\xd8jpg")
    write(s / "__pycache__" / "net.cpython-312.pyc", b"pyc")
    write(s / "._net.py", b"appledouble")
    write(s / ".DS_Store", b"ds")
    return s


@pytest.fixture
def vol(tmp_path):
    v = tmp_path / "TUFTY"
    (v / "apps").mkdir(parents=True)
    write(v / "apps" / "other_app" / "__init__.py", b"keep me")
    write(v / "main.py", b"keep me too")
    return v


def files(root):
    return sorted(str(p.relative_to(root)) for p in root.rglob("*") if p.is_file())


def test_fresh_install_copies_only_app_files(src, vol, no_dot_clean):
    r = I.install(src, vol, out=lambda m: None)
    t = vol / "apps" / "video_streamer"
    assert files(t) == ["__init__.py", "assets/a.jpg", "net.py", "wifi_config.py"]
    assert r["verified"] == 4 and not r["deleted"]
    assert (vol / "apps" / "other_app" / "__init__.py").read_bytes() == b"keep me"
    assert (vol / "main.py").read_bytes() == b"keep me too"


def test_mirror_deletes_stale_files_and_appledouble(src, vol, no_dot_clean):
    t = vol / "apps" / "video_streamer"
    write(t / "net.py", b"old")
    write(t / "__init__.py", b"app")  # unchanged
    write(t / "._net.py", b"appledouble")
    write(t / "old.py", b"gone")
    write(t / "__pycache__" / "x.pyc", b"pyc")
    write(t / "olddir" / "deep" / "f.txt", b"gone")
    write(t / "assets" / "._a.jpg", b"appledouble")
    r = I.install(src, vol, out=lambda m: None)
    assert files(t) == ["__init__.py", "assets/a.jpg", "net.py", "wifi_config.py"]
    assert not (t / "olddir").exists() and not (t / "__pycache__").exists()
    assert (t / "net.py").read_bytes() == b"net"
    assert "__init__.py" in r["same"] and "net.py" in r["copied"]
    assert I.verify(src, t) == []


def test_dry_run_changes_nothing(src, vol, no_dot_clean):
    t = vol / "apps" / "video_streamer"
    write(t / "old.py", b"gone")
    lines = []
    r = I.install(src, vol, dry_run=True, out=lines.append)
    assert files(t) == ["old.py"]
    assert "  delete old.py" in lines and "  copy   wifi_config.py" in lines
    assert len(r["copied"]) == 4


def test_refuses_without_wifi_config(src, vol, no_dot_clean):
    (src / "wifi_config.py").unlink()
    with pytest.raises(I.InstallError, match="wifi_config.py missing"):
        I.install(src, vol, out=lambda m: None)
    assert not (vol / "apps" / "video_streamer").exists()
    assert I.main(["--source", str(src), "--no-reset", "--volume", str(vol)]) == 1


def test_refuses_volume_without_apps(src, tmp_path):
    with pytest.raises(I.InstallError, match="not a Badgeware volume"):
        I.install(src, tmp_path / "empty", out=lambda m: None)


def test_refuses_symlinked_target(src, vol, tmp_path):
    elsewhere = tmp_path / "elsewhere"
    write(elsewhere / "precious.txt", b"x")
    os.symlink(elsewhere, vol / "apps" / "video_streamer")
    with pytest.raises(I.InstallError, match="symlinked"):
        I.install(src, vol, out=lambda m: None)
    assert (elsewhere / "precious.txt").exists()


def test_symlink_inside_target_is_removed_not_followed(src, vol, tmp_path, no_dot_clean):
    elsewhere = tmp_path / "elsewhere"
    write(elsewhere / "precious.txt", b"x")
    t = vol / "apps" / "video_streamer"
    t.mkdir()
    os.symlink(elsewhere, t / "link")
    I.install(src, vol, out=lambda m: None)
    assert not (t / "link").exists() and (elsewhere / "precious.txt").exists()


def test_verify_reports_mismatch_and_extra(src, vol, no_dot_clean):
    I.install(src, vol, out=lambda m: None)
    t = vol / "apps" / "video_streamer"
    write(t / "net.py", b"corrupt")
    write(t / "extra.py", b"x")
    assert I.verify(src, t) == ["hash mismatch net.py", "extra extra.py"]


def test_main_no_reset_installs_and_ejects(src, vol, monkeypatch, no_dot_clean):
    monkeypatch.setattr(I.sys, "platform", "darwin")
    ejected = []
    monkeypatch.setattr(I, "volume_ready", lambda p: p == str(vol))
    monkeypatch.setattr(I, "eject", ejected.append)
    monkeypatch.setattr(I.time, "sleep", lambda s: None)
    monkeypatch.setattr(I, "reset_into_msc", lambda port: pytest.fail("must not reset"))
    assert I.main(["--source", str(src), "--no-reset", "--volume", str(vol)]) == 0
    assert ejected == [vol]
    assert (vol / "apps" / "video_streamer" / "wifi_config.py").exists()


def test_wait_for_volume_times_out_and_settles():
    t = [0.0]
    sleeps = []

    def sleep(s):
        sleeps.append(s)
        t[0] += s
    with pytest.raises(I.InstallError, match="within 1 s"):
        I.wait_for_volume(("/nope",), timeout=1, ready=lambda p: False, sleep=sleep, clock=lambda: t[0])
    seen = []
    v = I.wait_for_volume(("/a", "/b"), timeout=5, sleep=sleep, clock=lambda: t[0],
                          ready=lambda p: seen.append(p) or (p == "/b" and len(seen) > 2))
    assert str(v) == "/b" and sleeps[-1] == I.SETTLE_S


def test_diskutil_ready_parsing():
    ok = "   Mounted:                   Yes\n   Volume Read-Only:          No\n"
    assert I.diskutil_ready(ok)
    assert not I.diskutil_ready(ok.replace("Read-Only:          No", "Read-Only:          Yes (read-only media)"))
    assert not I.diskutil_ready(ok.replace("Yes", "No", 1))


IOREG = '''+-o Tufty 2350@01100000  <class IOUSBHostDevice, id 0x1>
    {
      "USB Product Name" = "Tufty 2350"
    }
    +-o IOUSBHostInterface@0
      +-o IOSerialBSDClient  <class IOSerialBSDClient>
          {
            "IOCalloutDevice" = "/dev/cu.usbmodem2101"
          }
+-o USB Keyboard@01200000  <class IOUSBHostDevice, id 0x2>
    {
      "USB Product Name" = "Keyboard"
    }
      +-o IOSerialBSDClient  <class IOSerialBSDClient>
          {
            "IOCalloutDevice" = "/dev/cu.usbmodem9"
          }
'''


def test_ports_from_ioreg():
    assert I.ports_from_ioreg(IOREG) == ["/dev/cu.usbmodem2101"]
    assert I.ports_from_ioreg(IOREG, product="Nope") == []
    assert I.ioreg_ports(IOREG) == (["/dev/cu.usbmodem2101"], ["/dev/cu.usbmodem9"])


ONLY_XIAO = IOREG[IOREG.index("+-o USB Keyboard"):].replace("Keyboard", "USB JTAG/serial debug unit")


def test_find_port_prefers_the_tufty():
    assert I.find_port(IOREG, ["/dev/cu.usbmodem2101", "/dev/cu.usbmodem9"], darwin=True) == "/dev/cu.usbmodem2101"


def test_find_port_never_falls_back_to_another_devices_port():
    with pytest.raises(I.InstallError, match="other USB serial: /dev/cu.usbmodem9"):
        I.find_port(ONLY_XIAO, ["/dev/cu.usbmodem9"], darwin=True)


def test_find_port_fallback_skips_ports_of_other_products():
    # A port ioreg did not list at all may still be the badge; the XIAO's is excluded.
    assert I.find_port(ONLY_XIAO, ["/dev/cu.usbmodem9", "/dev/cu.usbmodem1"], darwin=True) == "/dev/cu.usbmodem1"


def test_find_port_glob_only_without_ioreg():
    assert I.find_port(None, ["/dev/cu.usbmodem1"], darwin=False) == "/dev/cu.usbmodem1"
    with pytest.raises(I.InstallError, match="several"):
        I.find_port(None, ["/dev/cu.usbmodem1", "/dev/cu.usbmodem2"], darwin=False)
    with pytest.raises(I.InstallError, match="no badge serial port"):
        I.find_port(None, [], darwin=False)


# -- final AppleDouble pass ------------------------------------------------

@pytest.fixture
def no_dot_clean(monkeypatch):
    monkeypatch.setattr(I.shutil, "which", lambda name: None)


def recreating_apply(monkeypatch, names):
    """Wrap apply() so ._ files appear after the copy, as macOS does on FAT."""
    real = I.apply

    def apply(src, target, copy, del_files, del_dirs):
        real(src, target, copy, del_files, del_dirs)
        for n in names:
            write(target / n, b"appledouble")
        write(target.parent / "._video_streamer", b"appledouble")
    monkeypatch.setattr(I, "apply", apply)


def test_appledouble_recreated_after_copy_is_removed_before_verify(src, vol, monkeypatch, no_dot_clean):
    recreating_apply(monkeypatch, ["._net.py", "._wifi_config.py", "assets/._a.jpg"])
    lines = []
    r = I.install(src, vol, out=lines.append)
    t = vol / "apps" / "video_streamer"
    assert files(t) == ["__init__.py", "assets/a.jpg", "net.py", "wifi_config.py"]
    assert not (vol / "apps" / "._video_streamer").exists()
    assert r["appledouble_removed"] == 4 and "  removed 4 AppleDouble files" in lines
    assert I.appledouble_files(t) == []
    # Other apps' AppleDouble files are not ours to touch.
    write(vol / "apps" / "other_app" / "._x.py", b"appledouble")
    I.install(src, vol, out=lambda m: None)
    assert (vol / "apps" / "other_app" / "._x.py").exists()


def test_dot_clean_runs_on_the_app_folder_when_available(src, vol, monkeypatch):
    calls = []

    def run(cmd, **kw):
        calls.append(cmd)
        return I.subprocess.CompletedProcess(cmd, 0, "", "")
    monkeypatch.setattr(I.shutil, "which", lambda name: "/usr/bin/dot_clean" if name == "dot_clean" else None)
    monkeypatch.setattr(I.subprocess, "run", run)
    recreating_apply(monkeypatch, ["._net.py"])
    I.install(src, vol, out=lambda m: None)
    assert calls == [["dot_clean", "-m", str(vol / "apps" / "video_streamer")]]
    assert I.appledouble_files(vol / "apps" / "video_streamer") == []


def test_dot_clean_failure_still_removes_files(src, vol, monkeypatch):
    def run(cmd, **kw):
        raise OSError("boom")
    monkeypatch.setattr(I.shutil, "which", lambda name: "/usr/bin/dot_clean")
    monkeypatch.setattr(I.subprocess, "run", run)
    recreating_apply(monkeypatch, ["._net.py"])
    lines = []
    I.install(src, vol, out=lines.append)
    assert any(m.startswith("  dot_clean failed") for m in lines)
    assert I.appledouble_files(vol / "apps" / "video_streamer") == []


def test_verify_fails_if_appledouble_survives(src, vol, monkeypatch, no_dot_clean):
    recreating_apply(monkeypatch, ["._net.py"])
    monkeypatch.setattr(I, "final_cleanup", lambda target, out=None: 0)
    ejected = []
    monkeypatch.setattr(I, "eject", ejected.append)
    with pytest.raises(I.InstallError, match=r"AppleDouble left \.\./\._video_streamer; AppleDouble left \._net\.py"):
        I.install(src, vol, out=lambda m: None)
    assert ejected == []


def test_cleanup_runs_after_copy_and_before_verify(src, vol, monkeypatch, no_dot_clean):
    order = []
    real_apply, real_clean, real_verify = I.apply, I.final_cleanup, I.verify
    monkeypatch.setattr(I, "apply", lambda *a: (order.append("apply"), real_apply(*a))[1])
    monkeypatch.setattr(I, "final_cleanup", lambda t, out=None: (order.append("cleanup"), real_clean(t, out=out))[1])
    monkeypatch.setattr(I, "verify", lambda s, t: (order.append("verify"), real_verify(s, t))[1])
    I.install(src, vol, out=lambda m: None)
    assert order == ["apply", "cleanup", "verify"]


def test_dry_run_skips_cleanup(src, vol, monkeypatch):
    monkeypatch.setattr(I, "final_cleanup", lambda *a, **k: pytest.fail("dry run must not clean"))
    I.install(src, vol, dry_run=True, out=lambda m: None)


@pytest.fixture
def stock_vol(vol):
    write(vol / "apps/menu/__init__.py", b"stock launcher")
    write(vol / "apps/menu/assets/icon.png", b"stock icon")
    write(vol / "apps/second_app/nested/file.bin", b"\x00\xfforiginal bytes")
    (vol / "apps/second_app/empty").mkdir()
    write(vol / "apps/loose.mpy", b"loose app")
    write(vol / "settings.json", b"private settings")
    return vol


def test_only_app_archives_exact_bytes_and_preserves_menu_and_system(src, stock_vol, tmp_path, no_dot_clean):
    backup = tmp_path / "archive"
    saved_menu = I.tree_manifest(stock_vol / "apps/menu")
    original = {p.name: I.tree_manifest(p) for p in (stock_vol / "apps").iterdir() if p.name != "menu"}
    result = I.install(src, stock_vol, only_app=True, backup_dir=backup, out=lambda m: None)
    assert sorted(p.name for p in (stock_vol / "apps").iterdir()) == ["menu", "video_streamer"]
    assert I.tree_manifest(stock_vol / "apps/menu") == saved_menu
    assert (stock_vol / "main.py").read_bytes() == b"keep me too"
    assert (stock_vol / "settings.json").read_bytes() == b"private settings"
    assert result["archived"] == sorted(original)
    assert result["backup"] == backup
    assert {p.name: I.tree_manifest(p) for p in (backup / "apps").iterdir()} == original
    assert I.verify(src, stock_vol / "apps/video_streamer") == []


def test_only_app_dry_run_never_copies_deletes_or_makes_backup(src, stock_vol, tmp_path, monkeypatch):
    backup = tmp_path / "archive"
    before = I.tree_manifest(stock_vol)
    monkeypatch.setattr(I, "apply", lambda *a: pytest.fail("dry run must not copy"))
    monkeypatch.setattr(I, "archive_other_apps", lambda *a, **k: pytest.fail("dry run must not archive"))
    result = I.install(src, stock_vol, only_app=True, backup_dir=backup, dry_run=True, out=lambda m: None)
    assert I.tree_manifest(stock_vol) == before
    assert not backup.exists()
    assert result["archived"] == ["loose.mpy", "other_app", "second_app"]


def test_only_app_requires_a_working_stock_menu(src, vol, tmp_path):
    backup = tmp_path / "archive"
    for empty_menu in (False, True):
        if empty_menu:
            (vol / "apps/menu").mkdir()
        with pytest.raises(I.InstallError, match="stock apps/menu launcher"):
            I.install(src, vol, only_app=True, backup_dir=backup, out=lambda m: None)
        assert not (vol / "apps/video_streamer").exists()
        assert not backup.exists()


def test_only_app_accepts_compiled_stock_menu(src, vol, tmp_path, no_dot_clean):
    write(vol / "apps/menu/__init__.mpy", b"stock compiled launcher")
    I.install(src, vol, only_app=True, backup_dir=tmp_path / "archive", out=lambda m: None)
    assert (vol / "apps/menu/__init__.mpy").read_bytes() == b"stock compiled launcher"


@pytest.mark.parametrize("location", ["volume", "inside", "existing"])
def test_only_app_refuses_unsafe_backup_location_before_changes(src, stock_vol, tmp_path, location):
    if location == "volume":
        backup = stock_vol
    elif location == "inside":
        backup = stock_vol / "backup"
    else:
        backup = tmp_path / "archive"
        write(backup / "precious.txt", b"untouched")
    before = I.tree_manifest(stock_vol)
    with pytest.raises(I.InstallError, match="already exists|outside the badge"):
        I.install(src, stock_vol, only_app=True, backup_dir=backup, out=lambda m: None)
    assert I.tree_manifest(stock_vol) == before
    if location == "existing":
        assert (backup / "precious.txt").read_bytes() == b"untouched"


def test_only_app_requires_backup_and_backup_flag_requires_only_app(src, stock_vol, tmp_path):
    with pytest.raises(I.InstallError, match="requires --backup-dir"):
        I.install(src, stock_vol, only_app=True, out=lambda m: None)
    with pytest.raises(I.InstallError, match="requires --only-app"):
        I.install(src, stock_vol, backup_dir=tmp_path / "backup", out=lambda m: None)
    assert not (stock_vol / "apps/video_streamer").exists()


@pytest.mark.parametrize("nested", [False, True])
def test_only_app_refuses_any_symlink_in_apps(src, stock_vol, tmp_path, nested):
    other = tmp_path / "other"
    write(other / "precious.txt", b"untouched")
    link = stock_vol / "apps/other_app/link" if nested else stock_vol / "apps/link"
    link.symlink_to(other, target_is_directory=True)
    with pytest.raises(I.InstallError, match="symlinked"):
        I.install(src, stock_vol, only_app=True, backup_dir=tmp_path / "archive", out=lambda m: None)
    assert (other / "precious.txt").read_bytes() == b"untouched"
    assert not (stock_vol / "apps/video_streamer").exists()
    assert (stock_vol / "apps/other_app/__init__.py").exists()


def test_only_app_refuses_symlinked_backup_parent(src, stock_vol, tmp_path):
    real = tmp_path / "real"
    real.mkdir()
    link = tmp_path / "linked"
    link.symlink_to(real, target_is_directory=True)
    with pytest.raises(I.InstallError, match="symlinked backup"):
        I.install(src, stock_vol, only_app=True, backup_dir=link / "archive", out=lambda m: None)
    assert not (real / "archive").exists()
    assert not (stock_vol / "apps/video_streamer").exists()


def test_refuses_symlink_in_source(src, vol, tmp_path):
    write(tmp_path / "outside.py", b"outside source")
    (src / "linked.py").symlink_to(tmp_path / "outside.py")
    with pytest.raises(I.InstallError, match="symlinked"):
        I.install(src, vol, out=lambda m: None)
    assert not (vol / "apps/video_streamer").exists()


def test_only_app_streamer_verification_precedes_backup_and_removal(src, stock_vol, tmp_path, monkeypatch, no_dot_clean):
    backup = tmp_path / "archive"
    monkeypatch.setattr(I, "verify", lambda *a: ["simulated bad app"])
    with pytest.raises(I.InstallError, match="verify failed"):
        I.install(src, stock_vol, only_app=True, backup_dir=backup, out=lambda m: None)
    assert not backup.exists()
    assert (stock_vol / "apps/other_app/__init__.py").read_bytes() == b"keep me"
    assert (stock_vol / "apps/second_app/nested/file.bin").read_bytes() == b"\x00\xfforiginal bytes"


@pytest.mark.parametrize("failure", ["copy", "corrupt"])
def test_only_app_no_partial_deletion_when_any_backup_fails(src, stock_vol, tmp_path, monkeypatch, no_dot_clean, failure):
    backup = tmp_path / "archive"
    originals = {p.name: I.tree_manifest(p) for p in (stock_vol / "apps").iterdir()}
    real = I.shutil.copyfile

    def copyfile(source, dest, **kwargs):
        if pathlib.Path(dest) == backup / "apps/second_app/nested/file.bin":
            if failure == "copy":
                raise OSError("simulated disk full")
            result = real(source, dest, **kwargs)
            pathlib.Path(dest).write_bytes(b"corrupted archive")
            return result
        return real(source, dest, **kwargs)

    monkeypatch.setattr(I.shutil, "copyfile", copyfile)
    with pytest.raises(I.InstallError, match="archive|backup verification failed"):
        I.install(src, stock_vol, only_app=True, backup_dir=backup, out=lambda m: None)
    for name, expected in originals.items():
        assert I.tree_manifest(stock_vol / "apps" / name) == expected
    assert I.verify(src, stock_vol / "apps/video_streamer") == []


def test_main_dry_run_only_app_does_not_discover_reset_or_eject(src, stock_vol, tmp_path, monkeypatch, no_dot_clean):
    monkeypatch.setattr(I.sys, "platform", "linux")
    monkeypatch.setattr(I, "find_port", lambda: pytest.fail("must not discover"))
    monkeypatch.setattr(I, "reset_into_msc", lambda *a: pytest.fail("must not reset"))
    monkeypatch.setattr(I, "wait_for_volume", lambda *a: pytest.fail("must not wait"))
    monkeypatch.setattr(I, "eject", lambda *a: pytest.fail("must not eject"))
    backup = tmp_path / "archive"
    before = I.tree_manifest(stock_vol)
    assert I.main(["--source", str(src), "--volume", str(stock_vol), "--dry-run", "--only-app",
                   "--backup-dir", str(backup)]) == 0
    assert I.tree_manifest(stock_vol) == before
    assert not backup.exists()


@pytest.mark.parametrize("platform", ["linux", "win32"])
@pytest.mark.parametrize("no_reset", [False, True])
def test_main_unsupported_platform_fails_before_device_access(src, vol, monkeypatch, capsys, platform, no_reset):
    monkeypatch.setattr(I.sys, "platform", platform)
    monkeypatch.setattr(I, "find_port", lambda: pytest.fail("must not discover"))
    monkeypatch.setattr(I, "reset_into_msc", lambda *a: pytest.fail("must not reset"))
    monkeypatch.setattr(I, "wait_for_volume", lambda *a: pytest.fail("must not wait"))
    monkeypatch.setattr(I, "install", lambda *a, **k: pytest.fail("must not copy"))
    args = ["--source", str(src), "--volume", str(vol)]
    if no_reset:
        args.append("--no-reset")
    assert I.main(args) == 1
    error = capsys.readouterr().err
    assert "macOS only" in error and "Disk Mode" in error
