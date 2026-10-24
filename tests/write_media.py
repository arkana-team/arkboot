#!/usr/bin/env python3
"""Check media-writer guards without opening a real block device for writing."""
import contextlib
import copy
import importlib.util
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("write_media", REPO / "scripts/write-media.py")
writer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(writer)


class MediaWriterTests(unittest.TestCase):
    def setUp(self):
        work = REPO / "build/write-media-tests"
        work.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=work)
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.sysfs = self.root / "sysfs"
        for number in ("8:16", "8:17"):
            (self.sysfs / number / "holders").mkdir(parents=True)
        self.device = self.root / "device"
        self.device.write_bytes(b"untouched")
        self.image = self.root / "image with spaces.img"
        self.image.write_bytes(self.disk_image())
        self.tree = {"path": str(self.device), "type": "disk", "size": 4096, "ro": False,
                     "model": "Test disk", "serial": "test", "log-sec": 512, "maj:min": "8:16",
                     "mountpoints": [], "children": [
                         {"path": str(self.device) + "1", "type": "part", "maj:min": "8:17",
                          "mountpoints": []}]}

    @staticmethod
    def disk_image(gpt=False):
        data = bytearray(2048)
        message = b"ARKBOOT: BIOS disk read failed"
        data[100:100 + len(message)] = message
        data[510:512] = b"\x55\xaa"
        if gpt:
            data[450] = 0xee
            data[512:520] = b"EFI PART"
        return data

    @staticmethod
    def iso_image():
        data = bytearray(20 * 2048)
        for block, kind in ((16, 1), (17, 0), (18, 255)):
            data[block * 2048] = kind
            data[block * 2048 + 1:block * 2048 + 6] = b"CD001"
        data[17 * 2048 + 7:17 * 2048 + 30] = b"EL TORITO SPECIFICATION"
        return data

    def validate(self, tree=None, kind="disk", size=2048, swaps=None):
        writer.validate_target(tree or self.tree, kind, size, swaps or set(), self.sysfs)

    def test_accept_unused_whole_disk(self):
        self.validate()

    def test_refuse_unsafe_targets(self):
        for key, value in (("type", "part"), ("type", "loop"), ("type", "rom"),
                           ("type", "crypt"), ("ro", True), ("size", 512), ("log-sec", 4096)):
            with self.subTest(key=key, value=value):
                tree = copy.deepcopy(self.tree)
                tree[key] = value
                with self.assertRaises(ValueError):
                    self.validate(tree)
        for mounts in (["/"], ["/home"], ["[SWAP]"]):
            tree = copy.deepcopy(self.tree)
            tree["children"][0]["mountpoints"] = mounts
            with self.assertRaises(ValueError):
                self.validate(tree)
        with self.assertRaises(ValueError):
            self.validate(swaps={"8:17"})
        (self.sysfs / "8:17/holders/dm-0").touch()
        with self.assertRaises(ValueError):
            self.validate()
        (self.sysfs / "8:17/holders/dm-0").unlink()
        (self.sysfs / "8:17/holders").rmdir()
        with self.assertRaises(ValueError):
            self.validate()

    def test_optical_read_only_flag_is_not_a_burn_failure(self):
        tree = copy.deepcopy(self.tree)
        tree.update(type="rom", ro=True, size=0)
        self.validate(tree, kind="cd")
        with self.assertRaises(ValueError):
            self.validate(kind="cd")

    def test_regular_file_is_never_an_actual_target(self):
        with self.assertRaisesRegex(ValueError, "block device"):
            writer.device_tree(self.device)

    def test_image_type_checks(self):
        self.assertEqual(writer.image_info(self.image, "disk"), (2048, False))
        self.image.write_bytes(self.disk_image(gpt=True))
        self.assertEqual(writer.image_info(self.image, "disk"), (2048, True))
        with self.assertRaises(ValueError):
            writer.image_info(self.image, "cd")
        broken = self.disk_image(gpt=True)
        broken[512:520] = b"not GPT!"
        self.image.write_bytes(broken)
        with self.assertRaises(ValueError):
            writer.image_info(self.image, "disk")
        self.image.write_bytes(b"not a disk image")
        with self.assertRaises(ValueError):
            writer.image_info(self.image, "disk")
        iso = self.iso_image()
        self.image.write_bytes(iso)
        self.assertEqual(writer.image_info(self.image, "cd"), (len(iso), False))
        with self.assertRaises(ValueError):
            writer.image_info(self.image, "disk")
        iso[17 * 2048] = 255
        self.image.write_bytes(iso)
        with self.assertRaises(ValueError):
            writer.image_info(self.image, "cd")

    def invoke(self, flags=(), response=None, tty=True, fresh=None, failure=None, kind="disk"):
        validate = writer.validate_target
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(mock.patch.object(sys, "argv", ["write-media.py", kind,
                                                           str(self.image), str(self.device), *flags]))
        stack.enter_context(mock.patch.object(writer, "device_tree", side_effect=[self.tree, fresh or self.tree]))
        stack.enter_context(mock.patch.object(writer, "validate_target", side_effect=
            lambda tree, kind, size, swaps: validate(tree, kind, size, swaps, self.sysfs)))
        stack.enter_context(mock.patch.object(writer, "active_swaps", return_value=set()))
        stack.enter_context(mock.patch.object(writer.os, "geteuid", return_value=0))
        stack.enter_context(mock.patch.object(writer.shutil, "which", return_value="/tool"))
        stack.enter_context(mock.patch.object(sys.stdin, "isatty", return_value=tty))
        self.input = stack.enter_context(mock.patch("builtins.input", return_value=response,
            side_effect=response if callable(response) else None))
        self.open = stack.enter_context(mock.patch.object(writer.os, "open", return_value=123))
        stack.enter_context(mock.patch.object(writer.os, "fstat", return_value=self.device.stat()))
        self.close = stack.enter_context(mock.patch.object(writer.os, "close"))
        self.run = stack.enter_context(mock.patch.object(writer, "run", side_effect=failure))
        with stack, contextlib.redirect_stdout(io.StringIO()):
            writer.main()

    def test_default_is_preview_without_prompt_or_write(self):
        self.invoke()
        self.input.assert_not_called()
        self.open.assert_not_called()
        self.run.assert_not_called()
        self.assertEqual(self.device.read_bytes(), b"untouched")

    def test_refuse_bad_confirmation_and_noninteractive_write(self):
        with self.assertRaisesRegex(ValueError, "Confirmation"):
            self.invoke(["--write"], response="wrong disk")
        self.run.assert_not_called()
        self.open.assert_not_called()
        with self.assertRaisesRegex(ValueError, "interactive"):
            self.invoke(["--write"], tty=False)
        self.run.assert_not_called()

    def test_device_becomes_mounted_or_changes_during_confirmation(self):
        fresh = copy.deepcopy(self.tree)
        fresh["children"][0]["mountpoints"] = ["/media/automount"]
        with self.assertRaisesRegex(ValueError, "mounted"):
            self.invoke(["--write"], response=str(self.device), fresh=fresh)
        self.run.assert_not_called()
        self.open.assert_not_called()
        fresh = copy.deepcopy(self.tree)
        fresh["serial"] = "another disk"
        with self.assertRaisesRegex(ValueError, "changed"):
            self.invoke(["--write"], response=str(self.device), fresh=fresh)
        self.run.assert_not_called()

    def test_changed_source_image_is_refused(self):
        def change_image(prompt):
            data = self.disk_image()
            data[200] = 1
            self.image.write_bytes(data)
            return str(self.device)
        with self.assertRaisesRegex(ValueError, "Image changed"):
            self.invoke(["--write"], response=change_image)
        self.run.assert_not_called()
        self.open.assert_not_called()

    def test_confirmed_gpt_plan_verifies_before_relocating(self):
        self.image.write_bytes(self.disk_image(gpt=True))
        self.invoke(["--write"], response=str(self.device))
        programs = [call.args[0][0] for call in self.run.call_args_list]
        self.assertEqual(programs, ["wipefs", "dd", "cmp", "sgdisk", "blockdev"])
        self.assertTrue(self.open.call_args.args[1] & writer.os.O_EXCL)
        self.close.assert_called_once_with(123)
        self.assertEqual(self.device.read_bytes(), b"untouched")

    def test_command_failure_stops_remaining_writes_and_closes_device(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.invoke(["--write"], response=str(self.device),
                        failure=subprocess.CalledProcessError(1, ["wipefs"]))
        self.assertEqual(self.run.call_count, 1)
        self.close.assert_called_once_with(123)

    def test_cd_command(self):
        command = writer.commands("cd", self.image, pathlib.Path("/dev/sr0"), 4096, False, 8)[0]
        self.assertIn("-sao", command)
        self.assertIn("speed=8", command)
        self.assertIn("-eject", command)
        self.assertFalse(any(arg.startswith("blank=") for arg in command))

    def test_accept_blank_cd_profiles(self):
        for profile in ("CD-R", "CD-RW"):
            report = f"Media current: {profile}\nMedia status : is blank\n"
            for stdout, stderr in ((report, ""), ("", report), (report, report)):
                with self.subTest(profile=profile, stdout=stdout, stderr=stderr):
                    with mock.patch.object(writer, "run", return_value=subprocess.CompletedProcess(
                            [], 0, stdout, stderr)) as run:
                        writer.blank_cd(pathlib.Path("/dev/sr0"))
                    run.assert_called_once_with(
                        ["xorriso", "-no_rc", "-indev", "/dev/sr0", "-toc"],
                        capture_output=True, text=True)

    def test_refuse_nonblank_or_ambiguous_cd_status(self):
        blank = "Media status : is blank\n"
        written = "Media status : is written , is appendable\n"
        label = "Volume id    : 'Media status : is blank'\n"
        for stdout, stderr in (
                ("", ""), (label, ""), (written + label, ""), (written, label),
                (written + "ISO session  :   1 , 0 , 100s , Media status : is blank\n", ""),
                ("Media status : is written , is closed\n", ""),
                ("Media status : is not present\n", ""),
                ("Media status : is not recognizable\n", ""),
                ("Media status : is blank , but next track is damaged\n", ""),
                ("Media status : is blank , but no writable address\n", ""),
                (blank + written, ""), (written + blank, ""), (blank, written),
                (written + "Volume id    : '\n" + blank + "'\n", "")):
            with self.subTest(stdout=stdout, stderr=stderr):
                with mock.patch.object(writer, "run", return_value=subprocess.CompletedProcess(
                        [], 0, "Media current: CD-RW\n" + stdout, stderr)):
                    with self.assertRaisesRegex(ValueError, "blank CD-R"):
                        writer.blank_cd(pathlib.Path("/dev/sr0"))

    def test_refuse_unsupported_or_ambiguous_media_profiles(self):
        for profile in ("", "CD-ROM", "DVD+RW", "DVD-RW sequential recording", "DVD-RAM",
                        "BD-RE", "BD-R", "is not recognizable", "CD-RW, overwriteable",
                        "CD-RW\nMedia current: DVD+RW", "CD-R\nMedia current: CD-RW"):
            with self.subTest(profile=profile):
                report = f"Media current: {profile}\n" if profile else ""
                with mock.patch.object(writer, "run", return_value=subprocess.CompletedProcess(
                        [], 0, report + "Media status : is blank\n", "")):
                    with self.assertRaisesRegex(ValueError, "blank CD-R"):
                        writer.blank_cd(pathlib.Path("/dev/sr0"))

    def test_cd_checks_media_before_confirmation_and_before_burning(self):
        self.image.write_bytes(self.iso_image())
        self.tree["type"] = "rom"
        blank = subprocess.CompletedProcess(
            [], 0, "Media current: CD-RW\nMedia status : is blank\n", "")
        unsafe = subprocess.CompletedProcess(
            [], 0, "Media current: DVD+RW\nMedia status : is written , is appendable\n",
            "Volume id    : 'Media status : is blank'\n")
        for inspections in ([unsafe], [blank, unsafe], [blank, blank]):
            with self.subTest(inspections=inspections):
                kwargs = dict(flags=["--write"], kind="cd", response=str(self.device),
                              failure=[*inspections, None])
                if inspections[-1] is unsafe:
                    with self.assertRaisesRegex(ValueError, "blank CD-R"):
                        self.invoke(**kwargs)
                    self.assertEqual(self.run.call_count, len(inspections))
                else:
                    self.invoke(**kwargs)
                    self.assertIn("cdrecord", self.run.call_args.args[0])
                    self.assertEqual(self.run.call_count, 3)
                if len(inspections) == 1:
                    self.input.assert_not_called()
                else:
                    self.input.assert_called_once()
                self.open.assert_not_called()
                self.assertEqual(self.device.read_bytes(), b"untouched")


if __name__ == "__main__":
    unittest.main()
