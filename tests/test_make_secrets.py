"""Credential generation uses temporary directories and never existing secrets."""
import importlib.util
import pathlib
import re
import stat

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("make_secrets", ROOT / "tools/make_secrets.py")
S = importlib.util.module_from_spec(spec)
spec.loader.exec_module(S)


@pytest.fixture
def targets(tmp_path, monkeypatch):
    targets = {"esp": tmp_path / "esp/main/wifi_secrets.h",
               "tufty": tmp_path / "tufty/video_streamer/wifi_config.py"}
    monkeypatch.setattr(S, "TARGETS", targets)
    return targets


def test_one_private_pair_only(targets, capsys):
    assert S.main([]) == 0
    esp = targets["esp"].read_text()
    badge = targets["tufty"].read_text()
    password = re.search(r'#define STREAM_AP_PASSWORD "([A-Za-z0-9]{20})"', esp).group(1)
    assert '#define STREAM_AP_SSID "tufty-video"' in esp
    assert 'SSID = "tufty-video"' in badge and 'PASSWORD = "%s"' % password in badge
    assert all(stat.S_IMODE(path.stat().st_mode) == 0o600 for path in targets.values())
    assert password not in capsys.readouterr().out
    assert sorted(p for p in targets["esp"].parents[2].rglob("*") if p.is_file()) == sorted(targets.values())


@pytest.mark.parametrize("existing", ["esp", "tufty"])
def test_refuses_even_one_existing_file_without_changing_either(targets, existing, capsys):
    target = targets[existing]
    target.parent.mkdir(parents=True)
    target.write_text("old private configuration")
    assert S.main([]) == 1
    assert target.read_text() == "old private configuration"
    assert not targets["tufty" if existing == "esp" else "esp"].exists()
    assert "old private" not in capsys.readouterr().out


def test_force_regenerates_both_files(targets):
    assert S.main([]) == 0
    before = {key: path.read_bytes() for key, path in targets.items()}
    assert S.main(["--force"]) == 0
    assert all(path.read_bytes() != before[key] for key, path in targets.items())


def test_refuses_symlink_even_with_force(targets, tmp_path):
    other = tmp_path / "other"
    other.write_text("leave untouched")
    targets["esp"].parent.mkdir(parents=True)
    targets["esp"].symlink_to(other)
    assert S.main(["--force"]) == 1
    assert other.read_text() == "leave untouched"
    assert not targets["tufty"].exists()


def test_staging_failure_does_not_replace_existing_pair(targets, monkeypatch):
    assert S.main([]) == 0
    before = {key: path.read_bytes() for key, path in targets.items()}
    real = S.tempfile.mkstemp
    count = [0]

    def failing(*args, **kwargs):
        count[0] += 1
        if count[0] == 2:
            raise OSError("simulated disk full")
        return real(*args, **kwargs)

    monkeypatch.setattr(S.tempfile, "mkstemp", failing)
    assert S.main(["--force"]) == 1
    assert all(path.read_bytes() == before[key] for key, path in targets.items())
    assert not list(targets["esp"].parent.glob(".wifi-secrets-*"))


def test_unknown_argument_is_not_silently_accepted(targets):
    with pytest.raises(SystemExit) as e:
        S.main(["--froce"])
    assert e.value.code == 2
    assert not any(path.exists() for path in targets.values())
