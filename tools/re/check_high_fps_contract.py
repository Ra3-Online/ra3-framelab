"""Read-only retail 1.12 high-FPS contracts (no game execution)."""
import argparse
import hashlib
import re
import struct
from pathlib import Path

from disasm import Image
from dryscan import compile_pattern, find_all, load_image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    image = Image(args.image)
    source = (Path(__file__).resolve().parents[2] / 'src/framelab.cpp').read_text(encoding='utf-8')
    raw, base, sections = load_image(args.image)
    sec = next(s for s in sections if s[0] == '.text')
    text = raw[sec[3]:sec[3]+sec[4]]
    count = 0

    def check(ok, label):
        nonlocal count
        if not ok:
            raise AssertionError(label)
        count += 1
        print('PASS', label)

    for name, expected in {
        'kSigHighEntry': [0x6027D0], 'kSigHighAdvance': [0x60282F],
        'kSigHighExit': [0x60294F], 'kSigHighLimiter': [0x629829],
        'kSigHighWindow': [0x5296CA, 0x52F527], 'kSigFpsGlobals': [0x5FFAD0],
        'kSigHighDrawableAlpha': [0x52F5FF],
        'kSigHighDrawableCaller': [0x53A790],
        'kSigHighTurretPrefix': [0x8FB6C5],
        'kSigHighTurretRead': [0x8FB6D2],
        'kSigHighTurretYaw': [0x8FB7BD],
        'kSigHighTurretPitch': [0x8FB91C],
        'kSigHighTurretSave': [0x8FBCAA],
        'kSigHighBoneAlpha': [0x8E8236],
        'kSigHighStructureAlpha': [0x8F3D85],
        'kSigDragScroll': [0xABFFB6],
        'kSigScrollStep1': [0xAC0263],
        'kSigScrollStep2': [0xABC9FC],
    }.items():
        match = re.search(r'\b'+name+r'\[\]\s*=\s*\{(.*?)\};', source, re.S)
        check(match is not None, name+' declared')
        body = re.sub(r'//[^\n]*', '', match.group(1))
        pat, mask = compile_pattern([t.strip() for t in body.split(',') if t.strip()])
        addresses = [base+sec[1]+i for i in find_all(text, pat, mask)]
        check(addresses == expected, name+' exact native match set')

    def code(va, expected, label):
        expected = bytes.fromhex(expected)
        check(image.read(va,len(expected)) == expected,label)

    code(0x6027ED,'8B 06 8B 90 94 00 00 00 8B CE FF D2', 'pre-update virtual call follows entry restore')
    code(0x52F5FF,'8B 15 8C 2F CE 00 D9 42 60', 'unit position/rotation reader consumes engine alpha before next advance')
    code(0x52F608,'51 8D 86 E8 00 00 00 D9 54 24 10 D9 1C 24', 'unit reader keeps one x87 value for native position/rotation interpolation')
    code(0x52F500,'83 EC 10 53 56', 'unit reader outer return begins after 16 locals and EBX/ESI')
    code(0x52F53E,'57', 'unit interpolation branch also saves EDI: outer return at hook ESP+0x20')
    code(0x8E823B,'F3 0F 10 40 60', 'attached bone reader writes XMM0 only, EAX owner load remains native')
    code(0x8F3D8B,'F3 0F 10 41 60', 'structure height reader writes XMM0 only, ECX owner load remains native')
    code(0x8FB6D0,'85 ED F3 0F 10 40 60 F3 0F 11 44 24 30 74 74', 'turret alpha load must preserve TEST EBP flags for following JE')
    code(0x8FB826,'D9 43 08 D9 1B F3 0F 10 44 24 14 F3 0F 11 43 08', 'native yaw capture keeps original endpoint writers')
    code(0x8FB933,'D9 43 0C D9 5B 04 F3 0F 10 44 24 10 F3 0F 11 43 0C', 'native pitch capture keeps original endpoint writers')
    code(0x8FBCA0,'F3 0F 10 44 24 30 8B 6C 24 74 F3 0F 11 43 10', 'turret cache marker writer is the final scalar cache field')
    code(0x8FB7D9,'0F 2F 05 88 14 C6 00', 'native yaw shortest-arc positive pi comparison remains outside patch')
    code(0x8FB944,'8B 47 08 8B 80 08 02 00 00 85 C0', 'pitch no-capture branch reaches original continuation')
    code(0xABFF9E,'F3 0F 10 05 38 03 CB 00 F3 0F 10 0D 3C 03 CB 00 F3 0F 59 C2 F3 0F 59 CA', 'right-drag native live axis/user settings precede scaled offset products')
    code(0xABFFCA,'8B 0D B4 B7 CD 00 8B 11 8B 52 50 8D 44 24 08 50 FF D2', 'right-drag preserves original local-vector view callback')
    code(0xABFFDE,'8D 44 24 08 50 8B CE E8 D6 C7 FF FF', 'right-drag retains original direction feedback callback')
    code(0xABFF6D,'72 17', 'native nonzero X branch still enters right-drag settings path')
    code(0xABFF84,'73 44', 'native zero-offset path still bypasses multiplication hook')
    code(0xC80F94,'40 FF AB 00', 'native right-drag virtual function pointer remains unmodified')
    code(0x60285C,'8D 41 01 F3 0F 2A C0 F3 0F 59 05 80 D3 C0 00', 'native +1 and simulation fraction stay intact')
    code(0x602888,'57', 'normal path pushes EDI once; display-only adapter must supply it')
    code(0x6028EE,'53 8B CE 89 5E 58 F3 0F 11 46 60 FF D0', 'new tick uses native phase 1 dispatch')
    code(0x602921,'89 7E 58', 'network refusal restores original out-of-range phase')
    code(0x60295D,'5F 5E 5B 83 C4 04 FF E0', 'exit pops EDI/ESI/EBX/local before tail render')
    code(0x616256,'A1 D4 F9 CA 00 33 D2 F7 35 D0 F9 CA 00', 'batch reader is native 30/15')
    ratio = bytes.fromhex('A1 D4 F9 CA 00 33 D2 F7 35 D0 F9 CA 00')
    actual = [base+sec[1]+i for i in range(len(text)-len(ratio)+1) if text.startswith(ratio,i)]
    check(actual == [0x5FFAD0,0x60283D,0x616256,0x61B59B,0x62935D],
          'complete five-reader ratio set matches runtime split policy')
    code(0x629745,'8B 4F 5C 03 0D 74 2F CE 00', 'native limiter consumes display-ratio field +0x5C')
    code(0x6162A0,'01 5D 58', 'native inner batch advances each remaining phase')
    code(0x6162B7,'F3 0F 2A 45 58 F3 0F 59 05 80 D3 C0 00', 'native post-batch fraction uses its exact constant')
    code(0x629825,'D9 6C 24 14', 'limiter sets native x87 truncate mode before replaced block')
    code(0x629833,'D9 6C 24 10', 'limiter restores native x87 control word after replaced block')
    check(struct.unpack('<f',image.read(0xC0D380,4))[0] == struct.unpack('<f',struct.pack('<f',1/6))[0],
          'native simulation alpha constant bit pattern')
    print(f'HIGH native contract: {count} passed; SHA256={hashlib.sha256(raw).hexdigest()}; game_executed=0')


if __name__ == '__main__':
    main()
