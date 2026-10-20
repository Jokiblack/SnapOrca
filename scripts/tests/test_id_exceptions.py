#!/usr/bin/env python3
"""Tests for the frozen id exceptions of scripts/orca_profile_tool.py
(scripts/profile_id_exceptions.json): the Snapmaker Orca 2.4 ids the tree keeps.

Run from the repo root:  python -m unittest discover -s scripts/tests -v
"""
import contextlib
import io
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
sys.path.insert(0, os.path.abspath(os.path.dirname(__file__)))

import orca_profile_tool as afi  # noqa: E402
from test_setting_id import SettingTree, preset  # noqa: E402


class ExceptionsCase(unittest.TestCase):
    """A synthetic tree plus an exceptions file of its own, in place of the shipped one."""

    def setUp(self):
        self.t = SettingTree()
        self.addCleanup(self.t.cleanup)
        self.exceptions = os.path.join(self.t.dir, "profile_id_exceptions.json")
        self._saved = afi.ID_EXCEPTIONS_PATH
        afi.ID_EXCEPTIONS_PATH = self.exceptions
        afi._id_exceptions.cache_clear()

        def restore():
            afi.ID_EXCEPTIONS_PATH = self._saved
            afi._id_exceptions.cache_clear()
        self.addCleanup(restore)

    def keep(self, vendor, setting_id=None, filament_id=None):
        with open(self.exceptions, "w", encoding="utf-8") as f:
            json.dump({"vendors": {vendor: {"setting_id": setting_id or {},
                                            "filament_id": filament_id or {}}}}, f)
        afi._id_exceptions.cache_clear()

    def quiet(self, fn, *args, **kwargs):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            result = fn(*args, **kwargs)
        return result, buf.getvalue()


class TestSettingIdExceptions(ExceptionsCase):
    def write_tree(self):
        self.t.write("Snap", "filament", preset(
            "Snap PLA @U1", setting_id="1195313935011", filament_vendor=["Snap"],
            filament_type=["PLA"]))
        self.t.write("Snap", "filament", preset(
            "Snap PLA @U1 0.2 nozzle", setting_id="1195313935011", filament_vendor=["Snap"],
            filament_type=["PLA"]))
        self.t.write("Snap", "process", preset(
            "fdm_process_common", instantiation=False, setting_id="2598417433",
            type_name="process"))

    def test_a_listed_id_passes_and_is_not_rewritten(self):
        self.write_tree()
        self.keep("Snap", setting_id={
            "filament/Snap PLA @U1.json": "1195313935011",
            "filament/Snap PLA @U1 0.2 nozzle.json": "1195313935011",
            "process/fdm_process_common.json": "2598417433"})
        errors, out = self.quiet(afi.check_setting_id_uniqueness, self.t.profiles)
        self.assertEqual(errors, 0, out)
        (changed, errors), out = self.quiet(afi.generate_setting_ids, self.t.profiles,
                                            dry_run=True)
        self.assertEqual((changed, errors), (0, 0), out)
        errors, out = self.quiet(afi.check_id_exceptions, self.t.profiles)
        self.assertEqual(errors, 0, out)

    def test_an_unlisted_file_is_held_to_the_rules(self):
        self.write_tree()
        self.keep("Snap", setting_id={"filament/Snap PLA @U1.json": "1195313935011"})
        errors, out = self.quiet(afi.check_setting_id_uniqueness, self.t.profiles)
        # The 0.2 preset's id is not the mint of its name, and the base carries one.
        self.assertEqual(errors, 2, out)
        self.assertIn("Snap PLA @U1 0.2 nozzle.json does not match the expected", out)
        self.assertIn("base profile Snap/process/fdm_process_common.json", out)

    def test_a_listed_file_with_another_id_is_held_to_the_rules(self):
        self.write_tree()
        self.keep("Snap", setting_id={"filament/Snap PLA @U1.json": "999",
                                      "filament/Snap PLA @U1 0.2 nozzle.json": "1195313935011",
                                      "process/fdm_process_common.json": "2598417433"})
        errors, out = self.quiet(afi.check_setting_id_uniqueness, self.t.profiles)
        self.assertEqual(errors, 1, out)
        self.assertIn('setting_id "1195313935011" in Snap/filament/Snap PLA @U1.json', out)
        errors, out = self.quiet(afi.check_id_exceptions, self.t.profiles)
        self.assertEqual(errors, 1, out)
        self.assertIn('Snap/filament/Snap PLA @U1.json no longer carries setting_id "999"', out)


class TestFilamentIdExceptions(ExceptionsCase):
    def test_the_loader_falls_back_to_a_kept_setting_id(self):
        # PresetBundle::commit_vendor_preset: a system filament resolving no
        # filament_id takes its own setting_id.
        self.t.write("Snap", "filament", preset(
            "Snap PLA @U1", setting_id="1195313935011", filament_vendor=["Snap"],
            filament_type=["PLA"]))
        self.keep("Snap", setting_id={"filament/Snap PLA @U1.json": "1195313935011"})
        analysis = afi.analyze_tree(self.t.profiles)
        self.assertEqual(analysis["missing_effective"], [])
        rec = analysis["vendors"]["Snap"]["Snap PLA @U1"]
        self.assertEqual((rec["eff_filament_id"], rec["id_source"]),
                         ("1195313935011", "setting_id"))
        errors, out = self.quiet(afi.check_filament_ids, self.t.profiles)
        self.assertEqual(errors, 0, out)

    def test_an_unlisted_setting_id_is_no_filament_id(self):
        self.t.write("Snap", "filament", preset(
            "Snap PLA @U1", setting_id="1195313935011", filament_vendor=["Snap"],
            filament_type=["PLA"]))
        self.keep("Snap")
        analysis = afi.analyze_tree(self.t.profiles)
        self.assertEqual([name for _v, name, _f in analysis["missing_effective"]],
                         ["Snap PLA @U1"])

    def test_a_kept_filament_id_passes_and_is_not_reminted(self):
        self.t.write("Snap", "filament", preset(
            "Snap ABS @U1 base", instantiation=False, filament_id="1682237920",
            filament_vendor=["Snap"], filament_type=["ABS"]))
        self.t.write("Snap", "filament", preset(
            "Snap ABS @U1", inherits="Snap ABS @U1 base", setting_id="937533070"))
        self.keep("Snap", setting_id={"filament/Snap ABS @U1.json": "937533070"},
                  filament_id={"filament/Snap ABS @U1 base.json": "1682237920"})
        errors, out = self.quiet(afi.check_filament_ids, self.t.profiles)
        self.assertEqual(errors, 0, out)
        errors, out = self.quiet(afi.check_filament_id_length, self.t.profiles, "Snap")
        self.assertEqual(errors, 0, out)
        (changed, errors), out = self.quiet(afi.generate_filament_ids, self.t.profiles,
                                            dry_run=True)
        self.assertEqual((changed, errors), (0, 0), out)

    def test_an_unlisted_filament_id_is_still_an_error(self):
        self.t.write("Snap", "filament", preset(
            "Snap ABS @U1", filament_id="1682237920", setting_id="937533070",
            filament_vendor=["Snap"], filament_type=["ABS"]))
        self.keep("Snap", setting_id={"filament/Snap ABS @U1.json": "937533070"})
        errors, out = self.quiet(afi.check_filament_ids, self.t.profiles)
        self.assertEqual(errors, 1, out)
        self.assertIn('filament_id "1682237920" (Snap) is not a minted "OF" id', out)
        errors, out = self.quiet(afi.check_filament_id_length, self.t.profiles, "Snap")
        self.assertEqual(errors, 1, out)


class TestShippedExceptions(unittest.TestCase):
    def test_the_shipped_list_names_only_snapmaker_files_that_still_carry_the_ids(self):
        exceptions = afi._id_exceptions()
        self.assertEqual(set(exceptions), {"Snapmaker"})
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = afi.check_id_exceptions(afi.PROFILES_DIR)
        self.assertEqual(errors, 0, buf.getvalue())


if __name__ == "__main__":
    unittest.main()
