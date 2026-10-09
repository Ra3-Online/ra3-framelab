"""Read-only checks of the supported image's visual layouts; never run the game.

Unlike the owned C++ mocks, these assertions inspect native machine code. Supply
your own image; an optional extracted retail glow asset checks its geometry type.
No game bytes or extracted asset are bundled in this repository.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct

from disasm import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--glow-asset", type=Path)
    args = parser.parse_args()
    image = Image(args.image)
    source = Path(__file__).resolve().parents[2] / "src" / "visual_timing.h"
    header = source.read_text(encoding="utf-8")
    checks = 0

    def check(ok, label):
        nonlocal checks
        if not ok:
            raise AssertionError(label)
        checks += 1
        print("PASS", label)

    def code(va, expected, label):
        expected = bytes.fromhex(expected)
        check(image.read(va, len(expected)) == expected, label)

    check(image.imagebase == 0x400000, "native image base")
    table = struct.unpack("<5I", image.read(0xC19244, 20))
    check(table == (0, 4, 5, 9, 8), "native GPU geometry vertex-count table")
    match = re.search(r"kFlGlowVertexCount\s*=\s*(\d+)", header)
    check(match is not None and int(match.group(1)) == table[2],
          "compiled retail centred-quad count agrees with native table entry 2")
    code(0x6C1710, "8B 41 08 8B 40 28 C3", "GPU geometry getter reads draw-template +40")
    code(0x6D23C8, "8B 04 85 44 92 C1 00 89 46 24",
         "storage reads geometry table then writes count at +36")
    code(0x6C6C7F, "0F AF 43 24 8D 14 80 8B 44 24 1C 8D 14 D0",
         "upload buffer offset uses count times 40-byte records")
    code(0x6B99D0, "51 8B 54 24 08 8B 42 28 DB 42 28",
         "native writer converts integer particle lifetime at +40")
    writer = image.read(0x6B99D0, 0x90)
    check(bytes.fromhex("F3 0F 11 40 E4") in writer,
          "native writer stores lifetime at +12 (end pointer -28)")
    check(bytes.fromhex("F3 0F 11 48 F4") in writer,
          "native writer stores birth at +28 (end pointer -12)")
    check(bytes.fromhex("83 C0 28") in writer and bytes.fromhex("3B 71 24") in writer,
          "native writer advances by 40 bytes until storage vertex count")
    code(0x90F8A3, "A1 88 13 CE 00 3B 86 C8 00 00 00 74 07",
         "native render skips animation at equal stamp")
    state_change = image.read(0x90C1B0, 0x80)
    check(bytes.fromhex("89 86 C8 00 00 00") in state_change,
          "native state change stores current stamp in module +200")
    check(struct.unpack("<I", image.read(0xC62AF8, 4))[0] == 0xB727C5AC,
          "native initial previous-frame epsilon")
    code(0x90DF1D, "D8 0D F8 2A C6 00 D8 C1 D9 5C 24 08 D9 5C 24 04",
         "initial previous frame is current plus direction times native epsilon")
    code(0x8F4932, "89 8E 2C 01 00 00",
         "track initializer stores animation mode at module +300")
    code(0x6FD550, "B8 01 00 00 00 84 05 FC 5F CE 00 56 8B F1",
         "read-only visual progress query entry")
    if args.glow_asset:
        asset = args.glow_asset.read_bytes()
        check(len(asset) == 552 and hashlib.sha256(asset).hexdigest() ==
              "aa1665d73576be1e873ebe1494e40d16d59ab875a5ddb0921b7327733acaa147",
              "optional retail glow asset identity")
        draw_offset = struct.unpack_from("<I", asset, 144)[0]
        check(draw_offset == 300 and struct.unpack_from("<I", asset, draw_offset + 36)[0] == 2,
              "retail glow GPUDrawModule uses native geometry type 2")
    else:
        print("Retail asset checks NOT RUN: optionally pass --glow-asset with your own extracted asset.")
    print(f"native visual contract: {checks} passed, 0 failed")


if __name__ == "__main__":
    main()
