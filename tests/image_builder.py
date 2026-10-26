#!/usr/bin/env python3
"""Check boot-tree staging, including deterministic source substitution races."""
import contextlib
import importlib.util
import io
import os
import pathlib
import shutil
import stat
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("mkimage", REPO / "scripts/mkimage.py")
mkimage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkimage)


class ImageBuilderTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = pathlib.Path(self.tmp.name)
        self.root = self.work / "root"
        self.boot = self.root / "boot"
        self.boot.mkdir(parents=True)
        (self.boot / "arkboot.cfg").write_text("kernel=/boot/payload\ninitrd=\n")
        self.payload = self.boot / "payload"
        self.payload.write_bytes(b"inside")
        self.outside = self.work / "outside"
        self.outside.mkdir()
        (self.outside / "payload").write_bytes(b"outside secret")
        self.tree = self.work / "snapshot"

    def test_copy_preserves_contents_modes_and_timestamps(self):
        self.payload.chmod(0o751)
        os.utime(self.payload, ns=(1700000000000000000, 1700000001000000000))
        self.boot.chmod(0o750)
        mkimage.copy_root(self.root, self.tree)
        self.assertEqual((self.tree / "boot/payload").read_bytes(), b"inside")
        for original, copied in ((self.boot, self.tree / "boot"),
                                 (self.payload, self.tree / "boot/payload")):
            self.assertEqual(stat.S_IMODE(copied.stat().st_mode), stat.S_IMODE(original.stat().st_mode))
            self.assertEqual(copied.stat().st_mtime_ns, original.stat().st_mtime_ns)
        self.payload.write_bytes(b"changed")
        self.assertEqual((self.tree / "boot/payload").read_bytes(), b"inside")

    def test_copy_accepts_the_relative_root_used_by_make(self):
        previous = pathlib.Path.cwd()
        os.chdir(self.work)
        try:
            mkimage.copy_root(pathlib.Path("root"), pathlib.Path("relative-snapshot"))
        finally:
            os.chdir(previous)
        self.assertEqual((self.work / "relative-snapshot/boot/payload").read_bytes(), b"inside")

    def test_reject_symlinks_and_fifo(self):
        entry = self.root / "entry"
        for target in (self.outside, self.outside / "payload", self.work / "missing"):
            with self.subTest(target=target), tempfile.TemporaryDirectory(dir=self.work) as tmp:
                entry.symlink_to(target)
                with self.assertRaises(ValueError):
                    mkimage.copy_root(self.root, pathlib.Path(tmp) / "snapshot")
                entry.unlink()
        os.mkfifo(entry)
        with self.assertRaises(ValueError):
            mkimage.copy_root(self.root, self.tree)

    def test_reject_symlink_substitution_at_open(self):
        # Replace entries after their type check, immediately before os.open.
        for kind in ("file", "directory", "root", "ancestor"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                parent = pathlib.Path(tmp)
                root = parent / "root"
                (root / "boot").mkdir(parents=True)
                (root / "boot/payload").write_bytes(b"inside")
                victim = {"file": root / "boot/payload", "directory": root / "boot",
                          "root": root, "ancestor": parent}[kind]
                original_open = os.open
                swapped = False

                def racing_open(path, flags, *args, **kwargs):
                    nonlocal swapped
                    if not swapped and path == victim.name and kwargs.get("dir_fd") is not None:
                        swapped = True
                        victim.rename(victim.with_name(victim.name + "-old"))
                        victim.symlink_to(self.outside / "payload" if kind == "file" else self.outside)
                    return original_open(path, flags, *args, **kwargs)

                with mock.patch.object(mkimage.os, "open", side_effect=racing_open):
                    with self.assertRaises(OSError):
                        mkimage.copy_root(root, self.tree)
                self.assertTrue(swapped)
                # Restore the TemporaryDirectory path before its cleanup.
                victim.unlink()
                victim.with_name(victim.name + "-old").rename(victim)
                if self.tree.exists():
                    shutil.rmtree(self.tree)

    def test_reject_regular_file_substitution_at_open(self):
        original_open = os.open
        old = self.payload.with_name("payload-old")
        swapped = False

        def racing_open(path, flags, *args, **kwargs):
            nonlocal swapped
            if not swapped and path == self.payload.name and kwargs.get("dir_fd") is not None:
                swapped = True
                self.payload.rename(old)
                self.payload.write_bytes(b"untrusted replacement")
            return original_open(path, flags, *args, **kwargs)

        with mock.patch.object(mkimage.os, "open", side_effect=racing_open):
            with self.assertRaisesRegex(ValueError, "changed while being staged"):
                mkimage.copy_root(self.root, self.tree)
        self.assertTrue(swapped)
        self.assertEqual(old.read_bytes(), b"inside")
        self.assertEqual(self.payload.read_bytes(), b"untrusted replacement")
        shutil.rmtree(self.tree)

    def test_directory_descriptor_survives_path_replacement(self):
        original_listdir = os.listdir
        swapped = False

        def racing_listdir(descriptor):
            nonlocal swapped
            if not swapped and os.fstat(descriptor).st_ino == self.boot.stat().st_ino:
                swapped = True
                self.boot.rename(self.root / "original-boot")
                self.boot.symlink_to(self.outside)
            return original_listdir(descriptor)

        with mock.patch.object(mkimage.os, "listdir", side_effect=racing_listdir):
            mkimage.copy_root(self.root, self.tree)
        self.assertTrue(swapped)
        self.assertEqual((self.tree / "boot/payload").read_bytes(), b"inside")

    def test_file_descriptor_survives_path_replacement(self):
        original_fdopen = os.fdopen
        swapped = False

        def racing_fdopen(descriptor, *args, **kwargs):
            nonlocal swapped
            if not swapped and os.fstat(descriptor).st_ino == self.payload.stat().st_ino:
                swapped = True
                self.payload.unlink()
                self.payload.symlink_to(self.outside / "payload")
            return original_fdopen(descriptor, *args, **kwargs)

        with mock.patch.object(mkimage.os, "fdopen", side_effect=racing_fdopen):
            mkimage.copy_root(self.root, self.tree)
        self.assertTrue(swapped)
        self.assertEqual((self.tree / "boot/payload").read_bytes(), b"inside")

    def test_reject_in_place_file_changes_during_snapshot(self):
        original_fdopen = os.fdopen
        changed = False

        def racing_fdopen(descriptor, *args, **kwargs):
            nonlocal changed
            if not changed and os.fstat(descriptor).st_ino == self.payload.stat().st_ino:
                changed = True
                info = os.fstat(descriptor)
                self.payload.write_bytes(b"mutant")
                os.utime(self.payload, ns=(info.st_atime_ns, info.st_mtime_ns + 1))
            return original_fdopen(descriptor, *args, **kwargs)

        with mock.patch.object(mkimage.os, "fdopen", side_effect=racing_fdopen):
            with self.assertRaisesRegex(ValueError, "changed while being staged"):
                mkimage.copy_root(self.root, self.tree)
        self.assertTrue(changed)
        shutil.rmtree(self.tree)

    def test_copy_empty_and_large_files(self):
        (self.boot / "empty").touch()
        data = bytes(range(256)) * 8193
        self.payload.write_bytes(data)
        mkimage.copy_root(self.root, self.tree)
        self.assertEqual((self.tree / "boot/empty").read_bytes(), b"")
        self.assertEqual((self.tree / "boot/payload").read_bytes(), data)

    def test_sparse_growth_never_exceeds_opening_size(self):
        original_fdopen = os.fdopen
        for size in (0, 6, 2 * 1024 * 1024 + 17):
            for timing in ("before read", "during read"):
                with self.subTest(size=size, timing=timing), tempfile.TemporaryDirectory(dir=self.work) as tmp:
                    tree = pathlib.Path(tmp) / "snapshot"
                    self.payload.write_bytes(b"x" * size)
                    changed = False

                    def grow():
                        nonlocal changed
                        if not changed:
                            changed = True
                            with self.payload.open("r+b") as stream:
                                stream.truncate(size + 4 * 1024 * 1024)

                    def racing_fdopen(descriptor, *args, **kwargs):
                        source = original_fdopen(descriptor, *args, **kwargs)
                        if os.fstat(descriptor).st_ino == self.payload.stat().st_ino:
                            if timing == "before read":
                                grow()
                            else:
                                original_read = source.read

                                def racing_read(length=-1):
                                    data = original_read(length)
                                    grow()
                                    return data

                                source.read = racing_read
                        return source

                    with mock.patch.object(mkimage.os, "fdopen", side_effect=racing_fdopen):
                        with self.assertRaisesRegex(ValueError, "changed while being staged"):
                            mkimage.copy_root(self.root, tree)
                    self.assertTrue(changed)
                    self.assertLessEqual((tree / "boot/payload").stat().st_size, size)

    def test_reject_premature_eof(self):
        original_fdopen = os.fdopen

        def racing_fdopen(descriptor, *args, **kwargs):
            if os.fstat(descriptor).st_ino == self.payload.stat().st_ino:
                self.payload.write_bytes(b"")
            return original_fdopen(descriptor, *args, **kwargs)

        with mock.patch.object(mkimage.os, "fdopen", side_effect=racing_fdopen):
            with self.assertRaisesRegex(ValueError, "changed while being staged"):
                mkimage.copy_root(self.root, self.tree)
        self.assertEqual((self.tree / "boot/payload").stat().st_size, 0)

    def test_reject_fifo_substitution_without_blocking(self):
        original_open = os.open

        def racing_open(path, flags, *args, **kwargs):
            if path == "payload" and kwargs.get("dir_fd") is not None:
                self.payload.unlink()
                os.mkfifo(self.payload)
                self.assertTrue(flags & os.O_NONBLOCK)
            return original_open(path, flags, *args, **kwargs)

        with mock.patch.object(mkimage.os, "open", side_effect=racing_open):
            with self.assertRaises(ValueError):
                mkimage.copy_root(self.root, self.tree)

    def invoke(self, kind, flags=()):
        for name in ("boot.bin", "cdboot.bin", "loader.bin", "BOOTX64.EFI"):
            (self.work / name).write_bytes(bytes(512))
        arguments = ["mkimage.py", kind, "--root", str(self.root),
                     "--output", str(self.work / "image"), *flags]
        with mock.patch.object(mkimage, "REPO", self.work), mock.patch.object(sys, "argv", arguments), \
                contextlib.redirect_stdout(io.StringIO()):
            mkimage.main()

    def test_all_image_flows_consume_private_snapshot(self):
        cases = (("iso", ()), ("iso", ("--udf",)), ("iso", ("--uefi",)),
                 ("iso", ("--udf", "--uefi")), ("disk", ("--fs", "fat12", "--size-mib", "8")),
                 ("disk", ("--fs", "ext4", "--uefi", "--partition-table", "gpt")))
        for kind, flags in cases:
            with self.subTest(kind=kind, flags=flags):
                observed = []

                def fake_run(*args):
                    if "-o" in args or "-d" in args or "-s" in args:
                        if "-o" in args:
                            tree = pathlib.Path(args[-1])
                            pathlib.Path(args[args.index("-o") + 1]).touch()
                        elif "-d" in args:
                            tree = pathlib.Path(args[args.index("-d") + 1])
                        elif args[-1] == "::/":
                            tree = pathlib.Path(args[-2]).parent
                        else:
                            return  # EFI executable copied to the ESP.
                        self.assertNotEqual(tree, self.root)
                        self.assertEqual(stat.S_IMODE(tree.parent.stat().st_mode), 0o700)
                        self.payload.write_bytes(b"changed after snapshot")
                        self.assertEqual((tree / "boot/payload").read_bytes(), b"inside")
                        observed.append(tree)

                self.payload.write_bytes(b"inside")
                with mock.patch.object(mkimage, "tool", side_effect=lambda name: name), \
                        mock.patch.object(mkimage, "run", side_effect=fake_run):
                    self.invoke(kind, flags)
                self.assertTrue(observed)
                self.assertFalse(observed[0].exists())
                self.assertFalse((self.boot / "arkboot-cd.bin").exists())

    def test_missing_config_and_source_links_preserve_output(self):
        output = self.work / "image"
        output.write_bytes(b"previous image")
        (self.boot / "arkboot.cfg").unlink()
        for kind in ("disk", "iso"):
            with self.subTest(kind=kind), mock.patch.object(mkimage, "run") as run:
                with self.assertRaisesRegex(ValueError, "Boot tree must contain"):
                    self.invoke(kind)
                self.assertEqual(output.read_bytes(), b"previous image")
                run.assert_not_called()
        (self.boot / "arkboot.cfg").symlink_to(self.outside / "payload")
        with self.assertRaises(ValueError):
            self.invoke("iso")
        self.assertEqual(output.read_bytes(), b"previous image")


if __name__ == "__main__":
    unittest.main()
