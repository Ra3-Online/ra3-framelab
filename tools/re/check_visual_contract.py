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
    parser.add_argument("--wall-manifest", type=Path,
                        help="optional retail static.manifest (local, read-only)")
    parser.add_argument("--empire-glow-dir", type=Path,
                        help="optional local JapanPowerGlow.asset and JapanPowerPlantIdleLight.asset")
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
    def call_target(va, expected, label):
        opcode = image.read(va, 5)
        check(opcode[0] == 0xe8 and va + 5 + struct.unpack_from("<i", opcode, 1)[0] == expected,
              label)

    call_target(0x90D5E8, 0x90C3A0, "default initializer invokes native geometry setup")
    call_target(0x90CDD5, 0x9B2E50, "geometry setup invokes native shared-reference scene-add")
    code(0x90CDD0, "57 FF D0 8B C8", "scene-add receives EDI shared geometry ref and scene in ECX")
    code(0x90CE60, "83 EC 7C 53 55 8B E9 8B 5D EC",
         "MC trampoline copies whole nonrelative prologue and resumes at +5")
    code(0x90CF66, "8D BD 78 01 00 00 B9 0F 00 00 00 F3 A5",
         "MC receiver copies 15 flags to receiver +376 (module +400)")
    call_target(0x90CFB8, 0x90C3A0, "MC callback performs native geometry setup")
    call_target(0x90CFDB, 0x90C1B0, "MC callback performs native animation setup before returning")
    code(0x90CFE0, "5F 5E 5D 5B 83 C4 7C C2 0C 00",
         "MC callback restores nonvolatile registers and removes three stack arguments")
    call_target(0x90FA63, 0x8E9D10, "draw's opacity multiplier is supplied by native x87 getter")
    code(0x90FA68, "D9 44 24 10 32 DB D8 4C 24 58 DE C9",
         "native alpha caller consumes ST0 before opacity classification")
    code(0x90FAAF, "F3 0F 11 41 60", "native final geometry alpha is at +96")
    code(0x53E827, "66 89 96 54 01 00 00", "Drawable constructor stores creation flags at +340 before module setup")
    code(0x7E2872, "89 48 04", "Drawable base constructor stores ThingTemplate at +4")
    code(0x482B23, "83 7E 04 00", "template name lookup reads instance word +4 (ThingTemplate +8)")
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
    if args.empire_glow_dir:
        for name, sha, identifier, constant, lifetime in (
            ("JapanPowerGlow", "c6ce21cb524998a23f88b16088b0b8ea6fbdb35263f740d3e063090516e0c2ea",
             0x565063F6, "kFlJapanPowerGlowId", 15.0),
            ("JapanPowerPlantIdleLight", "cc5317fc1cf489e3116b6f2b355068ff181e9404a9e7f51282e3a45a2e4a60af",
             0x56C6A9AD, "kFlJapanPowerPlantLightId", 1.0),
        ):
            asset = (args.empire_glow_dir / (name + ".asset")).read_bytes()
            check(len(asset) == 552 and hashlib.sha256(asset).hexdigest() == sha,
                  "optional retail Empire asset identity: " + name)
            check(struct.unpack_from("<Iff", asset, 56) == (1, lifetime, lifetime)
                  and struct.unpack_from("<Iff", asset, 92) == (1, lifetime, lifetime)
                  and struct.unpack_from("<Iff", asset, 104) == (1, 1.0, 1.0),
                  "retail Empire fixed lifetime and matching emission countdown: " + name)
            draw_offset = struct.unpack_from("<I", asset, 144)[0]
            check(draw_offset == 300 and struct.unpack_from("<I", asset, draw_offset + 36)[0] == 2,
                  "retail Empire glow uses native centred-quad geometry: " + name)
            match = re.search(constant + r"\s*=\s*0x([0-9A-F]+)u", header)
            check(match is not None and int(match.group(1), 16) == identifier,
                  "compiled Empire template identity: " + name)
    else:
        print("Empire asset checks NOT RUN: optionally pass --empire-glow-dir with your own extracted assets.")
    if args.wall_manifest:
        manifest = args.wall_manifest.read_bytes()
        hdr = struct.unpack_from("<12I", manifest)
        check(hdr[0] == 0x00060100, "optional wall manifest version/endianness")
        names = 48 + hdr[3] * 48 + hdr[8] + hdr[9]
        required = {
            "AlliedWallPiece": 0x296799CF, "AlliedWallSegmentPiece": 0x09435832,
            "JapanWallPiece": 0xF8C50039, "JapanWallSegmentPiece": 0xBF93CE00,
            "SovietWallPiece": 0xA82CF003, "SovietWallSegmentPiece": 0x0895CAE6,
        }
        found = {}
        for i in range(hdr[3]):
            entry = struct.unpack_from("<12I", manifest, 48 + i * 48)
            start = names + entry[6]
            name = manifest[start:manifest.index(0, start)].decode("utf-8")
            if name.startswith("GameObject:") and name[11:] in required:
                found[name[11:]] = (entry[0], entry[1])
        for name, instance in required.items():
            check(found.get(name) == (0x942FFF2D, instance) and
                  f"0x{instance:08X}u" in header,
                  "compiled wall identity agrees with retail manifest: " + name)
    else:
        print("Retail wall IDs NOT RUN: optionally pass --wall-manifest with your own static.manifest.")
    print(f"native visual contract: {checks} passed, 0 failed")


if __name__ == "__main__":
    main()
