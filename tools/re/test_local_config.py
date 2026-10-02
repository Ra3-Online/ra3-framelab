"""Offline input/configuration checks; fixtures stay under the checkout's build dir."""

import importlib.util
import os
from pathlib import Path
import unittest
from unittest.mock import patch
import uuid

import local_config


class LocalConfigTests(unittest.TestCase):
    def setUp(self):
        self.fixture = local_config.REPO_ROOT / "build" / "portability-tests" / uuid.uuid4().hex
        self.fixture.mkdir(parents=True)
        self.game = self.fixture / "game"
        (self.game / "Data").mkdir(parents=True)
        self.image = self.game / "Data" / "ra3_1.12.game"
        self.image.write_bytes(b"offline input fixture")
        self.dump = self.fixture / "input.c"
        self.dump.write_text("//----- (00500000) sub_500000 -----\nint dword_CAF9D4;\n", encoding="utf-8")

    def test_inputs_require_configuration(self):
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaises(SystemExit):
                local_config.image_path()
            self.assertIsNone(local_config.ida_dump_path())
            with self.assertRaises(SystemExit):
                local_config.ida_dump_path(required=True)

    def test_environment_and_parameter_precedence(self):
        with patch.dict(os.environ, {"FLAB_GAME_ROOT": str(self.game)}, clear=True):
            self.assertEqual(local_config.image_path(), self.image)
        with patch.dict(os.environ, {"FLAB_IMAGE": str(self.image), "FLAB_IDA_DUMP": str(self.dump)}, clear=True):
            self.assertEqual(local_config.ida_dump_path(required=True), self.dump)
            self.assertEqual(local_config.image_path(self.dump), self.dump)
        with self.assertRaises(SystemExit):
            local_config.image_path(self.fixture / "missing.game")

    def test_input_flags_preserve_legacy_positionals(self):
        args, options = local_config.take_input_options(["0x500000", "--image", "image.game", "64", "--ida-dump", "dump.c"])
        self.assertEqual(args, ["0x500000", "64"])
        self.assertEqual(options, {"--image": "image.game", "--ida-dump": "dump.c"})
        with self.assertRaises(SystemExit):
            local_config.take_input_options(["--image"])

    def test_build_directory_override(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(local_config.build_directory(), local_config.REPO_ROOT / "build")
        custom = self.fixture / "alternate build"
        with patch.dict(os.environ, {"FLAB_BUILD_DIR": str(custom)}):
            self.assertEqual(local_config.build_directory(), custom)
        original_cwd = Path.cwd()
        try:
            os.chdir(self.fixture)
            with patch.dict(os.environ, {"FLAB_BUILD_DIR": "build/relative-config-test"}):
                self.assertEqual(local_config.build_directory(), local_config.REPO_ROOT / "build" / "relative-config-test")
        finally:
            os.chdir(original_cwd)

    def test_relocation(self):
        relocated = self.fixture / "relocated checkout" / "tools" / "re" / "local_config.py"
        relocated.parent.mkdir(parents=True)
        relocated.write_bytes(Path(local_config.__file__).read_bytes())
        spec = importlib.util.spec_from_file_location("relocated_config", relocated)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual(module.REPO_ROOT, relocated.parents[2])

    def test_symbols_are_optional_and_cache_is_checkout_local(self):
        import disasm
        with patch.object(disasm, "DUMP", None):
            self.assertEqual(disasm.load_symbols(), ({}, {}))
        with patch.object(disasm, "DUMP", self.dump):
            funcs, globals_ = disasm.load_symbols()
            self.assertEqual(funcs[0x500000], "sub_500000")
            self.assertEqual(globals_[0xCAF9D4], "dword_CAF9D4")
            self.assertEqual(disasm.load_symbols(), (funcs, globals_))


if __name__ == "__main__":
    unittest.main()
