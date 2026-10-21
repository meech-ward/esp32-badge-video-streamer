"""Pinned JPEG endpoint/queue staging preserves the managed source."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "esp/components/camera_source/prepare_driver.py"
DRIVER = ROOT / "esp/managed_components/espressif__esp32-camera/driver/cam_hal.c"
SPEC = importlib.util.spec_from_file_location("camera_driver_staging", SCRIPT)
PATCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCHER)


def test_unreviewed_driver_revision_is_rejected():
    with pytest.raises(ValueError, match="revision changed"):
        PATCHER.prepare(b"different camera driver")


@pytest.mark.skipif(not DRIVER.exists(), reason="ESP managed camera dependency is not installed")
def test_endpoint_and_queue_patch_has_no_diagnostic_changes():
    source = DRIVER.read_bytes()
    staged = PATCHER.prepare(source)
    assert 'frame_buffer_queue_len = 1;' in staged
    assert '#include "jpeg_endpoint.h"' in staged
    assert 'streamer_jpeg_endpoint(inbuf, length)' in staged
    assert 'cam_verify_jpeg_eoi(dma_buffer->buf, dma_buffer->len)' in staged
    assert 'eoi_probe_window' not in staged and 'search_forward' not in staged
    assert 'JPEG_EOI_BYTES' not in staged
    assert 'streamer_camera_capture_event' not in staged
    assert DRIVER.read_bytes() == source


@pytest.mark.skipif(not DRIVER.exists(), reason="ESP managed camera dependency is not installed")
def test_staging_cannot_overwrite_the_driver_and_rejects_changed_patch_site(monkeypatch):
    source = DRIVER.read_bytes()
    result = subprocess.run([sys.executable, str(SCRIPT), str(DRIVER), str(DRIVER)],
                            text=True, capture_output=True)
    assert result.returncode != 0 and "outside" in result.stderr
    assert DRIVER.read_bytes() == source
    changed = source.replace(b'frame_buffer_queue_len = cam_obj->frame_cnt - 1;', b'frame_buffer_queue_len = 99;')
    monkeypatch.setattr(PATCHER, "PINNED_SHA256", hashlib.sha256(changed).hexdigest())
    with pytest.raises(ValueError, match="patch site changed"):
        PATCHER.prepare(changed)
