"""Exercise the real GUI's read-only preflight with disposable installation fixtures.

No game executable is provided or run. Fixture .game files contain plain text;
the --auto case must reject the known loader before reaching CreateProcessW.
"""

import ctypes
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class GuiConflictTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="FPS 检查 ", dir=GUI.parent)
        self.root = Path(self.scratch.name).resolve()
        if self.root.parent != GUI.parent:
            raise RuntimeError("Fixture escaped the declared build directory")
        self.addCleanup(self.scratch.cleanup)
        self.data = self.root / "游戏 目录" / "Data"
        self.data.mkdir(parents=True)
        self.game = self.data / "ra3_1.12.game"
        self.game.write_bytes(b"Non-executable offline fixture; never launch")

    def add_loader(self, proxy=b"MZ\0" + "CnCFpsUnlocker.dll\0".encode("utf-16le"), name="d3d9.dll"):
        (self.data / name).write_bytes(proxy)
        (self.data / "CnCFpsUnlocker.dll").write_bytes(b"offline companion fixture")

    def run_gui(self, expected, selected=None, mode="--check-conflicts"):
        before = {p: hashlib.sha256(p.read_bytes()).digest()
                  for p in self.data.iterdir() if p.is_file() and p.stat().st_size < 1024 * 1024}
        env = dict(os.environ, FLAB_RUNTIME_DIR=str(self.root / "logs"))
        command = [str(GUI), mode, str(selected or self.data.parent)]
        if mode == "--auto":
            command.extend(["90", "1"])
        result = subprocess.run(command, capture_output=True, timeout=15, env=env,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        logs = list((self.root / "logs").glob("gui-*.log"))
        self.assertEqual(len(logs), 1)
        text = logs[0].read_text(encoding="utf-8-sig")
        self.assertEqual(result.returncode, expected, text)
        for path, digest in before.items():
            self.assertEqual(hashlib.sha256(path.read_bytes()).digest(), digest)
        self.assertNotIn("已启动游戏 pid", text)
        self.assertNotIn("补丁已生效", text)
        self.assertFalse(list((self.root / "logs").glob("*.dll")))
        return text

    def test_clean_install(self):
        self.assertIn("未发现已知", self.run_gui(0))

    def test_legacy_wide_reference_with_unaligned_offset(self):
        self.add_loader()
        self.assertIn("另一个帧率补丁", self.run_gui(1))

    def test_data_selection(self):
        self.add_loader()
        self.assertIn("CnCFpsUnlocker.dll", self.run_gui(1, self.data))

    def test_ascii_reference_is_case_insensitive(self):
        self.add_loader(b"MZ\0CNCFPSUNLOCKER.DLL\0", "dinput8.dll")
        self.assertIn("dinput8.dll", self.run_gui(1))

    def test_flat_install(self):
        self.add_loader()
        flat = self.root / "平铺 游戏"
        self.data.rename(flat)
        self.data = flat
        self.assertIn("另一个帧率补丁", self.run_gui(1, flat))

    def test_other_renderer_proxy_is_allowed(self):
        self.add_loader(b"MZ\0Other D3D9 renderer\0")
        self.run_gui(0)

    def test_orphan_reference_without_companion(self):
        (self.data / "d3d9.dll").write_bytes(b"MZ\0CnCFpsUnlocker.dll\0")
        self.run_gui(0)

    def test_partial_reference_is_not_a_loader_name(self):
        self.add_loader(b"MZ\0CnCFpsUnlocker.dll")
        self.run_gui(0)

    def test_root_proxy_is_not_beside_the_actual_executable(self):
        self.add_loader()
        (self.data / "d3d9.dll").rename(self.data.parent / "d3d9.dll")
        self.run_gui(0)

    def test_missing_game_directory(self):
        self.run_gui(2, self.root / "missing")

    def test_oversized_proxy_is_bounded(self):
        self.add_loader()
        with (self.data / "d3d9.dll").open("r+b") as f:
            f.truncate(64 * 1024 * 1024 + 1)
        self.assertIn("64 MiB", self.run_gui(1))

    def test_unreadable_proxy_reports_failure(self):
        self.add_loader()
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                     ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.CreateFileW(str(self.data / "d3d9.dll"), 0x80000000, 0, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            # Hash reads also need sharing, so exercise this one without run_gui's reads.
            env = dict(os.environ, FLAB_RUNTIME_DIR=str(self.root / "logs"))
            result = subprocess.run([str(GUI), "--check-conflicts", str(self.data.parent)],
                                    capture_output=True, timeout=15, env=env,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(result.returncode, 1)
            logs = list((self.root / "logs").glob("gui-*.log"))
            self.assertEqual(len(logs), 1)
            self.assertIn("无法检查自动加载器", logs[0].read_text(encoding="utf-8-sig"))
        finally:
            kernel.CloseHandle(handle)

    def test_launch_path_refuses_before_create_process(self):
        self.add_loader()
        self.assertIn("未启动游戏", self.run_gui(1, mode="--auto"))


if __name__ == "__main__":
    if os.name != "nt":
        raise SystemExit("GUI conflict tests require Windows")
    GUI = Path(sys.argv.pop(1) if len(sys.argv) > 1 else
               Path(__file__).resolve().parents[2] / "build" / "Ra3FpsTest.exe").resolve()
    if not GUI.is_file():
        raise SystemExit(f"GUI was not built: {GUI}")
    unittest.main()
