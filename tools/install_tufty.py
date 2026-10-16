#!/usr/bin/env python3
"""Install tufty/video_streamer onto a Tufty 2350 through its USB disk (macOS).

1. Reboot the badge into Disk Mode: `mpremote connect <port> exec
   "import powman; powman.reset_into_msc()"` (skipped with --no-reset).
2. Wait for /Volumes/TUFTY or /Volumes/Tufty2350 to be mounted read-write.
3. Mirror the app into <volume>/apps/video_streamer: changed files are copied
   without extended attributes, files not in the source are deleted (only
   inside that folder), and macOS `._*` files are removed.
4. sync, then a final AppleDouble pass: `dot_clean -m <app folder>` if
   available, delete any `._*` left in the app folder (and apps/._video_streamer),
   sync again. macOS can recreate `._*` files on the FAT volume after the
   copy, so this runs last, right before the check.
5. Verify every file by SHA-256 and that no `._*` remain, then eject.

Stdlib only, plus the `mpremote` CLI for step 1. --dry-run prints the plan
without touching the badge or the volume.

--only-app --backup-dir <new external folder> first installs and verifies the
streamer, then archives and verifies other apps before removing them. The stock
apps/menu launcher is required and preserved, as are firmware and settings.
"""
import argparse
import glob
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "tufty" / "video_streamer"
APP_NAME = "video_streamer"
VOLUMES = ("/Volumes/TUFTY", "/Volumes/Tufty2350")
PRODUCT = "Tufty 2350"
MOUNT_TIMEOUT_S = 60
SETTLE_S = 2
SKIP_DIRS = {"__pycache__"}
SKIP_FILES = {".DS_Store"}


class InstallError(Exception):
    pass


def skipped(name):
    return name.startswith("._") or name.endswith(".example.py") or name in SKIP_FILES


# -- port discovery ---------------------------------------------------------

def ioreg_ports(text, product=PRODUCT):
    """(matching, others): serial devices (/dev/cu.*) under USB devices whose
    product name contains `product`, and those under any other USB device.

    `text` is `ioreg -r -c IOUSBHostDevice -l -w0` output: one tree per USB
    device, the device's own properties before its children's.
    """
    found, others = [], []
    current = None
    for line in text.splitlines():
        if line.startswith("+-o "):
            current = None  # next top-level USB device
            continue
        m = re.search(r'"USB Product Name" = "([^"]*)"', line)
        if m:
            current = m.group(1)
            continue
        m = re.search(r'"IOCalloutDevice" = "([^"]*)"', line)
        if m:
            dest = found if current is not None and product in current else others
            if m.group(1) not in dest:
                dest.append(m.group(1))
    return found, others


def ports_from_ioreg(text, product=PRODUCT):
    return ioreg_ports(text, product)[0]


def run_ioreg():
    """ioreg output, or None when ioreg is unavailable or failed."""
    try:
        r = subprocess.run(["ioreg", "-r", "-c", "IOUSBHostDevice", "-l", "-w0"],
                           capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return r.stdout if r.returncode == 0 else None


def find_port(ioreg_text=None, candidates=None, darwin=None):
    """The badge's serial port. Never guesses a port ioreg attributes to another
    device: the XIAO is also a /dev/cu.usbmodem*, and opening it can reset the ESP.
    """
    if darwin is None:
        darwin = sys.platform == "darwin"
    if darwin and ioreg_text is None:
        ioreg_text = run_ioreg()
    if candidates is None:
        candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    if darwin and ioreg_text is not None:
        ports, others = ioreg_ports(ioreg_text)
        if len(ports) == 1:
            return ports[0]
        if len(ports) > 1:
            raise InstallError("several %s ports, pick one with --port: %s" % (PRODUCT, " ".join(ports)))
        # Only fall back to a port ioreg did not place under some other product.
        candidates = [p for p in candidates if p not in others]
        if others and not candidates:
            raise InstallError("no %s port found (other USB serial: %s); use --port, or --no-reset "
                               "if the badge is already in Disk Mode" % (PRODUCT, " ".join(others)))
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise InstallError("no badge serial port found; connect it by USB or use --port / --no-reset")
    raise InstallError("several usbmodem ports, pick one with --port: %s" % " ".join(candidates))


def reset_into_msc(port):
    if shutil.which("mpremote") is None:
        raise InstallError("mpremote not on PATH (e.g. `uv tool install mpremote`)")
    print("reset into Disk Mode via %s" % port)
    try:
        r = subprocess.run(["mpremote", "connect", port, "exec", "import powman; powman.reset_into_msc()"],
                           capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        print("  mpremote timed out (the badge may already be rebooting)")
        return
    if r.returncode:
        # The badge drops USB serial as it reboots, so mpremote usually reports an error.
        print("  mpremote exited %d (expected: the badge disconnects as it resets)" % r.returncode)


# -- volume -----------------------------------------------------------------

def diskutil_ready(info):
    """True if `diskutil info` text says mounted and writable."""
    mounted = re.search(r"^\s*Mounted:\s*Yes\s*$", info, re.M)
    rw = re.search(r"^\s*Volume Read-Only:\s*No\b", info, re.M)
    return bool(mounted and rw)


def volume_ready(path):
    if not os.path.ismount(path):
        return False
    try:
        r = subprocess.run(["diskutil", "info", path], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0 and diskutil_ready(r.stdout)


def wait_for_volume(candidates=VOLUMES, timeout=MOUNT_TIMEOUT_S, ready=None,
                    sleep=None, clock=time.monotonic, settle=SETTLE_S):
    ready = ready or volume_ready
    sleep = sleep or time.sleep
    end = clock() + timeout
    while True:
        for v in candidates:
            if ready(v):
                # Finder and Spotlight touch a freshly mounted disk; let that pass first.
                sleep(settle)
                return pathlib.Path(v)
        if clock() >= end:
            raise InstallError("no writable %s within %d s" % (" or ".join(candidates), timeout))
        sleep(0.5)


def eject(volume):
    r = subprocess.run(["diskutil", "eject", str(volume)], capture_output=True, text=True, timeout=60)
    if r.returncode:
        raise InstallError("eject failed: %s" % (r.stderr or r.stdout).strip())


# -- mirror -----------------------------------------------------------------

def source_files(src):
    """Relative paths of the files to install, sorted."""
    refuse_symlinks(src)
    files = []
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS and not d.startswith("._"))
        for name in sorted(filenames):
            if not skipped(name):
                files.append(os.path.relpath(os.path.join(dirpath, name), src))
    return files


def refuse_symlinks(path):
    """Never follow a link while installing or archiving application files."""
    path = pathlib.Path(path)
    if path.is_symlink():
        raise InstallError("refusing symlinked %s" % path)
    if path.is_dir():
        for dirpath, dirnames, filenames in os.walk(path):
            for name in dirnames + filenames:
                child = pathlib.Path(dirpath) / name
                if child.is_symlink():
                    raise InstallError("refusing symlinked %s" % child)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(65536), b""):
            h.update(block)
    return h.hexdigest()


def target_dir(volume):
    """<volume>/apps/video_streamer, refusing anything that could escape the volume."""
    volume = pathlib.Path(volume)
    if not (volume / "apps").is_dir():
        raise InstallError("%s has no apps/ folder; not a Badgeware volume" % volume)
    target = volume / "apps" / APP_NAME
    if (volume / "apps").is_symlink() or target.is_symlink():
        raise InstallError("refusing symlinked %s" % target)
    if target.exists() and not target.is_dir():
        raise InstallError("%s exists and is not a folder" % target)
    if target.resolve().parent != (volume / "apps").resolve() or target.resolve().name != APP_NAME:
        raise InstallError("refusing target %s" % target)
    return target


def plan(src, target):
    """(copy, same, delete_files, delete_dirs): relative paths."""
    src = pathlib.Path(src)
    if not (src / "wifi_config.py").is_file():
        raise InstallError("%s/wifi_config.py missing; run tools/make_secrets.py first" % src)
    files = source_files(src)
    copy, same = [], []
    for rel in files:
        t = target / rel
        if t.is_file() and not t.is_symlink() and t.stat().st_size == (src / rel).stat().st_size \
                and sha256(t) == sha256(src / rel):
            same.append(rel)
        else:
            copy.append(rel)
    keep = set(files)
    keep_dirs = {d for f in files for d in _parents(f)}
    del_files, del_dirs = [], []
    if target.is_dir():
        for dirpath, dirnames, filenames in os.walk(target, topdown=False):
            for name in filenames + [d for d in dirnames if os.path.islink(os.path.join(dirpath, d))]:
                rel = os.path.relpath(os.path.join(dirpath, name), target)
                if rel not in keep:
                    del_files.append(rel)
            for name in dirnames:
                rel = os.path.relpath(os.path.join(dirpath, name), target)
                if rel not in keep_dirs and not os.path.islink(os.path.join(dirpath, name)):
                    del_dirs.append(rel)
    return copy, same, sorted(del_files), del_dirs


def _parents(rel):
    d = os.path.dirname(rel)
    while d:
        yield d
        d = os.path.dirname(d)


def _inside(target, rel):
    p = (target / rel)
    real = pathlib.Path(os.path.realpath(p.parent)) / p.name
    tr = pathlib.Path(os.path.realpath(target))
    if tr != real and tr not in real.parents:
        raise InstallError("refusing path outside %s: %s" % (target, rel))
    return p


def apply(src, target, copy, del_files, del_dirs):
    src = pathlib.Path(src)
    for rel in del_files:
        _inside(target, rel).unlink()
    for rel in del_dirs:
        _inside(target, rel).rmdir()
    target.mkdir(exist_ok=True)
    for rel in copy:
        dst = _inside(target, rel)
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.is_symlink():
            dst.unlink()
        # copyfile copies bytes only: no xattrs, so macOS writes no ._ files.
        shutil.copyfile(src / rel, dst)


def appledouble_files(target):
    """`._*` files in the app folder, plus the folder's own apps/._<name>."""
    target = pathlib.Path(target)
    found = []
    own = target.parent / ("._" + target.name)
    if own.is_file() and not own.is_symlink():
        found.append(own)
    if target.is_dir():
        for dirpath, dirnames, filenames in os.walk(target):
            dirnames.sort()
            for name in sorted(filenames):
                if name.startswith("._"):
                    found.append(pathlib.Path(dirpath) / name)
    return found


def final_cleanup(target, which=None, run=None, out=print):
    """Remove AppleDouble files after the copy; returns how many were deleted.

    WHY a separate last pass: macOS writes `._*` companions onto FAT volumes
    asynchronously (seen on the badge: a first install left some, a second
    run found 7 more), so the mirror step's own deletion is not enough.
    """
    which = which or shutil.which
    run = run or subprocess.run
    target = pathlib.Path(target)
    if which("dot_clean"):
        try:
            r = run(["dot_clean", "-m", str(target)], capture_output=True, text=True, timeout=30)
            if r.returncode:
                out("  dot_clean exited %d (removing ._ files directly)" % r.returncode)
        except (OSError, subprocess.TimeoutExpired) as e:
            out("  dot_clean failed: %s (removing ._ files directly)" % e)
    removed = 0
    for p in appledouble_files(target):
        if p == target.parent / ("._" + target.name):
            p.unlink()
        else:
            _inside(target, os.path.relpath(p, target)).unlink()
        removed += 1
    if removed:
        out("  removed %d AppleDouble file%s" % (removed, "" if removed == 1 else "s"))
    return removed


def verify(src, target):
    """List of problems; empty when the target mirrors the source exactly."""
    src = pathlib.Path(src)
    files = source_files(src)
    problems = []
    for rel in files:
        t = target / rel
        if not t.is_file():
            problems.append("missing " + rel)
        elif sha256(t) != sha256(src / rel):
            problems.append("hash mismatch " + rel)
    extra = set()
    for dirpath, _, filenames in os.walk(target):
        for name in filenames:
            if not name.startswith("._"):
                extra.add(os.path.relpath(os.path.join(dirpath, name), target))
    for rel in sorted(extra - set(files)):
        problems.append("extra " + rel)
    for p in appledouble_files(target):
        problems.append("AppleDouble left " + os.path.relpath(p, target))
    return problems


def other_apps(volume, backup_dir):
    """Preflight app removal. This function never changes the volume or backup."""
    volume = pathlib.Path(volume)
    apps = volume / "apps"
    refuse_symlinks(apps)
    if volume.is_symlink():
        raise InstallError("refusing symlinked %s" % volume)
    menu = apps / "menu"
    if not menu.is_dir() or not any((menu / name).is_file()
                                     for name in ("__init__.py", "__init__.mpy")):
        raise InstallError("--only-app requires the stock apps/menu launcher; restore it first")
    if backup_dir is None:
        raise InstallError("--only-app requires --backup-dir pointing outside the badge volume")
    backup = pathlib.Path(backup_dir).expanduser().absolute()
    for ancestor in (backup, *backup.parents):
        if ancestor.is_symlink():
            raise InstallError("refusing symlinked backup path %s" % ancestor)
        if ancestor.exists() and not ancestor.is_dir():
            raise InstallError("backup parent is not a folder: %s" % ancestor)
    if backup.exists():
        raise InstallError("backup folder already exists; choose a new --backup-dir: %s" % backup)
    if backup.resolve() == volume.resolve() or volume.resolve() in backup.resolve().parents:
        raise InstallError("--backup-dir must be outside the badge volume")
    keep = {APP_NAME, "menu", "._" + APP_NAME, "._menu"}
    entries = sorted((p for p in apps.iterdir() if p.name not in keep), key=lambda p: p.name)
    return entries, backup


def tree_manifest(path):
    """Exact tree contents, including empty folders, with SHA-256 for each file."""
    path = pathlib.Path(path)
    refuse_symlinks(path)
    if path.is_file():
        return {".": ("file", path.stat().st_size, sha256(path))}
    if not path.is_dir():
        raise InstallError("cannot archive non-file application entry: %s" % path)
    result = {".": ("dir",)}
    for dirpath, dirnames, filenames in os.walk(path):
        for name in dirnames:
            child = pathlib.Path(dirpath) / name
            result[str(child.relative_to(path))] = ("dir",)
        for name in filenames:
            child = pathlib.Path(dirpath) / name
            if not child.is_file():
                raise InstallError("cannot archive non-file application entry: %s" % child)
            result[str(child.relative_to(path))] = ("file", child.stat().st_size, sha256(child))
    return result


def archive_other_apps(entries, backup, out=print):
    """Back up and verify *all* entries before deleting the first source entry.

    WHY two verification passes: a successful copy is insufficient evidence for
    deletion. Also recheck the source after copying so a changing volume cannot
    leave an incomplete archive accepted as a backup.
    """
    try:
        manifests = {entry.name: tree_manifest(entry) for entry in entries}
        # Other apps may contain private local settings; protect the archive.
        backup.mkdir(mode=0o700, parents=True, exist_ok=False)
        saved_apps = backup / "apps"
        saved_apps.mkdir()
        for entry in entries:
            dest = saved_apps / entry.name
            if entry.is_dir():
                shutil.copytree(entry, dest, copy_function=shutil.copyfile)
            else:
                shutil.copyfile(entry, dest)
        if hasattr(os, "sync"):
            os.sync()
        for entry in entries:
            expected = manifests[entry.name]
            if tree_manifest(saved_apps / entry.name) != expected or tree_manifest(entry) != expected:
                raise InstallError("backup verification failed for %s; no other apps removed" % entry)
        for entry in entries:
            if entry.is_dir():
                shutil.rmtree(entry)
            else:
                entry.unlink()
            out("  archived and removed " + entry.name)
    except OSError as e:
        raise InstallError("cannot archive other apps: %s; retain the backup and inspect the volume" % e) from e


def install(src, volume, dry_run=False, out=print, only_app=False, backup_dir=None):
    target = target_dir(volume)
    copy, same, del_files, del_dirs = plan(src, target)
    if backup_dir is not None and not only_app:
        raise InstallError("--backup-dir requires --only-app")
    entries, backup = other_apps(volume, backup_dir) if only_app else ([], None)
    for rel in copy:
        out("  copy   " + rel)
    for rel in del_files:
        out("  delete " + rel)
    for rel in del_dirs:
        out("  rmdir  " + rel + "/")
    for entry in entries:
        out("  archive %s -> %s" % (entry, backup / "apps" / entry.name))
    if dry_run:
        out("dry run: %d to copy, %d unchanged, %d to delete in %s"
            % (len(copy), len(same), len(del_files) + len(del_dirs), target))
        return {"target": target, "copied": copy, "same": same, "deleted": del_files + del_dirs,
                "archived": [p.name for p in entries], "backup": backup}
    apply(src, target, copy, del_files, del_dirs)
    if hasattr(os, "sync"):
        os.sync()
    cleaned = final_cleanup(target, out=out)
    if hasattr(os, "sync"):
        os.sync()
    problems = verify(src, target)
    if problems:
        raise InstallError("verify failed: " + "; ".join(problems))
    if only_app:
        # Recheck safety after install and before archiving: no other app is
        # removed until the streamer has passed its own byte-for-byte check.
        fresh_entries, fresh_backup = other_apps(volume, backup_dir)
        if fresh_entries != entries or fresh_backup != backup:
            raise InstallError("apps changed during install; no other apps removed")
        archive_other_apps(entries, backup, out=out)
        if hasattr(os, "sync"):
            os.sync()
    return {"target": target, "copied": copy, "same": same, "deleted": del_files + del_dirs,
            "verified": len(copy) + len(same), "appledouble_removed": cleaned,
            "archived": [p.name for p in entries], "backup": backup}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="badge serial port (default: auto-detect)")
    ap.add_argument("--no-reset", action="store_true", help="the volume is already mounted")
    ap.add_argument("--volume", help="mounted badge volume (default: %s)" % " or ".join(VOLUMES))
    ap.add_argument("--dry-run", action="store_true", help="print the plan; change nothing")
    ap.add_argument("--only-app", action="store_true", help="archive and remove other apps, preserving the stock menu")
    ap.add_argument("--backup-dir", help="new folder outside the badge volume; required with --only-app")
    ap.add_argument("--source", default=str(SOURCE), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    try:
        if not args.dry_run and sys.platform != "darwin":
            raise InstallError("automatic installation is macOS only; on other systems, "
                               "copy tufty/video_streamer into the badge apps folder in Disk Mode")
        if args.only_app != bool(args.backup_dir):
            raise InstallError("use --only-app and --backup-dir together")
        if not (pathlib.Path(args.source) / "wifi_config.py").is_file():
            raise InstallError("%s/wifi_config.py missing; run tools/make_secrets.py first" % args.source)
        candidates = (args.volume,) if args.volume else VOLUMES
        if args.dry_run:
            vol = next((v for v in candidates if os.path.isdir(v)), None)
            if vol is None:
                raise InstallError("dry run needs a mounted volume (%s)" % " or ".join(candidates))
            install(args.source, vol, dry_run=True, only_app=args.only_app, backup_dir=args.backup_dir)
            return 0
        if not args.no_reset:
            reset_into_msc(args.port or find_port())
        vol = wait_for_volume(candidates)
        print("volume %s" % vol)
        r = install(args.source, vol, only_app=args.only_app, backup_dir=args.backup_dir)
        if r["backup"]:
            print("archived %d other apps entries in %s" % (len(r["archived"]), r["backup"]))
        eject(vol)
        print("installed %s: %d copied, %d unchanged, %d deleted, %d verified, %d ._ removed; ejected %s"
              % (r["target"], len(r["copied"]), len(r["same"]), len(r["deleted"]), r["verified"],
                 r["appledouble_removed"], vol))
        print("open video_streamer from the launcher")
        return 0
    except InstallError as e:
        print("install_tufty: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
