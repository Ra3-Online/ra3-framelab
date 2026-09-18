#!/usr/bin/env python3
# tools/re/check_exports.py -- 核对 Ra3FrameLab.dll 的导出表是否与 src/framelab.def 一致。
#
# 2026-09-17 动画定位会话新增。起因是一个**静默**的构建故障:
#   framelab.def 里被我加了三行中文注释,link.exe 按 ANSI 读 .def,结果紧接着的那一行
#   (`FrameLabAnimRate`)被吃掉。该函数于是只剩 __declspec(dllexport) 在起作用,
#   按 __stdcall 装饰成 `_FrameLabAnimRate@4` 导出。构建退出码 0、产物时间戳刷新、
#   大小也正常 —— 全部"通过"。直到运行期 flctl 的 GetProcAddress("FrameLabAnimRate")
#   返回 NULL,read/animrate 一律 -9999,才暴露出来。
#
# 教训写进检查里:.def 是导出名的唯一真源,构建后必须**逐名核对**导出表,
#   而且必须确认没有 `_Name@N` 这种装饰名混进来。
#
# 用法: check_exports.py <dll> <def>
#   退出码 0 = 一致;1 = 有问题(缺名/多出装饰名/解析失败)。

import re
import struct
import sys


def parse_def(path):
    """从 .def 里取 EXPORTS 段的名字(跳过 ; 注释与空行)。"""
    names = []
    in_exports = False
    with open(path, "rb") as f:
        raw = f.read()
    # .def 必须是纯 ASCII:非 ASCII 字节正是本次故障的根因,直接报错而不是猜。
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as e:
        raise SystemExit("FAIL: %s 含非 ASCII 字节(偏移 %d)—— .def 必须是纯 ASCII" % (path, e.start))
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith(";"):
            continue
        if not in_exports:
            if s.upper().startswith("EXPORTS"):
                in_exports = True
            continue
        # 只认「名字」或「名字 = 内部名」两种形态,取等号左边的导出名
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|$)", s)
        if m:
            names.append(m.group(1))
    return names


def parse_dll_exports(path):
    """解析 PE 导出表的名字列表(32/64 位都支持)。"""
    with open(path, "rb") as f:
        d = f.read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("FAIL: %s 不是 PE 文件" % path)
    coff = pe + 4
    nsec = struct.unpack_from("<H", d, coff + 2)[0]
    optsz = struct.unpack_from("<H", d, coff + 16)[0]
    opt = coff + 20
    is64 = struct.unpack_from("<H", d, opt)[0] == 0x20B
    ddoff = opt + (112 if is64 else 96)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        o = so + i * 40
        vs, va, rs, rp = struct.unpack_from("<IIII", d, o + 8)
        secs.append((va, vs, rp, rs))

    def r2o(rva):
        for va, vs, rp, rs in secs:
            if va <= rva < va + max(vs, rs):
                return rp + (rva - va)
        return None

    edir_rva = struct.unpack_from("<I", d, ddoff)[0]
    if edir_rva == 0:
        return []
    o = r2o(edir_rva)
    nnam = struct.unpack_from("<I", d, o + 24)[0]
    nof = struct.unpack_from("<I", d, o + 32)[0]
    names = []
    for i in range(nnam):
        rva = struct.unpack_from("<I", d, r2o(nof) + 4 * i)[0]
        no = r2o(rva)
        names.append(d[no:d.index(b"\0", no)].decode("latin1"))
    return names


def main():
    if len(sys.argv) < 3:
        raise SystemExit("用法: check_exports.py <dll> <def>")
    dll, deffile = sys.argv[1], sys.argv[2]

    want = parse_def(deffile)
    got = set(parse_dll_exports(dll))

    missing = [n for n in want if n not in got]
    # 装饰名(_Foo@4)= dllexport 抢在 .def 前面生效的指纹,说明 .def 那一行没被解析
    decorated = sorted(n for n in got if re.match(r"^_[A-Za-z_].*@\d+$", n))
    extra = sorted(got - set(want) - set(decorated))

    if missing or decorated or extra:
        if missing:
            print("FAIL: .def 里列了但 DLL 里没有(名字被装饰或 .def 没解析到):")
            for n in missing:
                print("   ", n)
        if decorated:
            print("FAIL: DLL 里有 __stdcall 装饰名 —— .def 对应行很可能没被解析:")
            for n in decorated:
                print("   ", n)
        if extra:
            print("FAIL: DLL 里多出 .def 未列的名字:")
            for n in extra:
                print("   ", n)
        return 1

    print("exports OK: %d 个名字与 %s 一致" % (len(want), deffile))
    return 0


if __name__ == "__main__":
    sys.exit(main())
