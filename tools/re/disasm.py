# re/disasm.py -- 按 VA 反汇编原始 .game(只读,不属于补丁本体)
#
# 2026-09-17 动画定位会话新增。为什么要它:
#   Hex-Rays 对 __thiscall 会**丢掉 ECX**(把 this 隐式化),于是转储里出现
#   `sub_99BE00(0)` 这种"看不见 this"的调用。要判断"哪个对象被读了",
#   只能回汇编看 ECX 是从哪儿来的。capstone 在本机隔离 venv 里已有(5.0.7)。
#
# 用法:
#   disasm.py <hexVA> [nbytes]          反汇编一段(默认 200 字节)
#   disasm.py <hexVA> --to-ret          一直反汇编到第一个 ret(含),上限 4KB
#   disasm.py <hexVA> --fn              用转储里的函数边界决定长度
#   disasm.py <hexVA> --n 64            等价于给 nbytes
#
# 标注规则:
#   - call/jmp 目标若在转储里有名字 => 显示 `sub_XXXXXX`
#   - 内存操作数里的绝对位移 => 显示 `[0x00XXXXXX]`;若该地址在 .data/.rdata 里,
#     再补一个 `=值`(4 字节小端,按整数/float 两种解释),方便一眼看出是不是全局
#   - 该地址若是转储里已知的全局名(dword_XXXXXX 之类)也补上名字
#
# 注意:capstone 的 32 位模式不吃 "imagebase 0x400000 即 VA" 这套,我们直接把 VA 当
# 地址喂给它,和 IDA 的显示保持一致 —— 因为转储里所有地址就是 VA(见交接文档)。
import io, os, re, struct, sys

IMAGE = r"G:\IDA\RA3_1.12.game"
DUMP  = r"G:\IDA\RA3_1.12.game.c"
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_symcache.txt")

DEF = re.compile(r"^//----- \(00([0-9A-F]+)\) (.*?) -----")
# 转储里的全局名: dword_CAF9D4 / byte_CE2F5B / flt_XXXX / off_XXXX / unk_XXXX / word_XXXX
GLOB = re.compile(r"\b(dword|byte|word|flt|dbl|off|unk|qword|stru)_([0-9A-F]{5,8})\b")


# ---------- 符号表 ----------
def load_symbols():
    """返回 (funcs, globs)。funcs: {va: name};globs: {va: name}。带磁盘缓存。"""
    if os.path.exists(CACHE):
        funcs, globs = {}, {}
        for line in io.open(CACHE, "r", encoding="utf-8"):
            kind, va, name = line.rstrip("\n").split("\t")
            (funcs if kind == "f" else globs)[int(va, 16)] = name
        return funcs, globs
    funcs, globs = {}, {}
    with io.open(DUMP, "r", encoding="utf-8", errors="replace") as f:
        for l in f:
            m = DEF.match(l)
            if m:
                funcs[int(m.group(1), 16)] = m.group(2)
            for kind, hexs in GLOB.findall(l):
                va = int(hexs, 16)
                if va >= 0x400000:
                    globs.setdefault(va, "%s_%s" % (kind, hexs))
    with io.open(CACHE, "w", encoding="utf-8") as f:
        for va, n in funcs.items():
            f.write("f\t%08X\t%s\n" % (va, n))
        for va, n in globs.items():
            f.write("g\t%08X\t%s\n" % (va, n))
    return funcs, globs


# ---------- PE ----------
def load_sections(data):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e_lfanew + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    optsize = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    imagebase = struct.unpack_from("<I", data, opt + 28)[0]
    sec = opt + optsize
    out = []
    for i in range(nsec):
        o = sec + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        out.append((name, vaddr, vsize, rawptr, rawsize))
    return imagebase, out


class Image(object):
    def __init__(self, path):
        self.data = open(path, "rb").read()
        self.imagebase, self.secs = load_sections(self.data)

    def read(self, va, n):
        rva = va - self.imagebase
        for name, vaddr, vsize, rawptr, rawsize in self.secs:
            if vaddr <= rva < vaddr + vsize:
                off = rawptr + (rva - vaddr)
                if off + n <= len(self.data):
                    return self.data[off:off + n]
        return None

    def is_in_file(self, va):
        """该 VA 是否落在有原始数据/被初始化的节里(.text/.rdata/.data),用于判断"能不能读出值"。"""
        rva = va - self.imagebase
        for name, vaddr, vsize, rawptr, rawsize in self.secs:
            if vaddr <= rva < vaddr + rawsize:
                return True, name
        return False, None


# ---------- 反汇编 ----------
def disasm(va, nbytes, img, funcs, globs, stop_at_ret=False):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_MEM, CS_OP_IMM
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    code = img.read(va, nbytes)
    if code is None:
        print("!! 0x%08X 不在可读节里" % va)
        return
    for ins in md.disasm(code, va):
        line = "  %08X  %-24s %s %s" % (ins.address, ins.bytes.hex().upper().ljust(24),
                                        ins.mnemonic, ins.op_str)
        note = []
        for op in ins.operands:
            if op.type == CS_OP_IMM:
                t = op.imm & 0xFFFFFFFF
                if t in funcs:
                    note.append("-> %s" % funcs[t])
                elif t in globs:
                    note.append("-> %s" % globs[t])
            elif op.type == CS_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                t = op.mem.disp & 0xFFFFFFFF
                nm = globs.get(t, "")
                ok, sec = img.is_in_file(t)
                extra = ""
                if ok and sec in (".data", ".rdata", ".text", ".bss"):
                    b = img.read(t, 4)
                    if b and len(b) == 4:
                        i = struct.unpack_from("<i", b, 0)[0]
                        f = struct.unpack_from("<f", b, 0)[0]
                        extra = " =%d/%.6g" % (i, f)
                note.append("[0x%08X]%s%s" % (t, (" " + nm) if nm else "", extra))
        if note:
            line = line.ljust(72) + "   ; " + "  ".join(note)
        print(line)
        if stop_at_ret and ins.mnemonic in ("ret", "retn") and ins.address + ins.size > va:
            print("  ---- 到 ret 为止 ----")
            return


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__ or "usage: disasm.py <hexVA> [nbytes] [--to-ret|--fn]")
        return
    va = int(args[0], 16)
    mode = "n"
    nbytes = 200
    rest = args[1:]
    if "--to-ret" in rest:
        mode, nbytes = "ret", 4096
    if "--fn" in rest:
        mode = "fn"
    for i, a in enumerate(rest):
        if a in ("--n",) and i + 1 < len(rest):
            nbytes = int(rest[i + 1])
        elif a.isdigit():
            nbytes = int(a)
    funcs, globs = load_symbols()
    if mode == "fn" and va in funcs:
        starts = sorted(funcs)
        import bisect
        k = bisect.bisect_right(starts, va)
        if k < len(starts):
            nbytes = min(starts[k] - va + 16, 8192)
            print("// 函数 %s 0x%08X,推断长度 %d 字节" % (funcs[va], va, nbytes))
    img = Image(IMAGE)
    print("// 反汇编 0x%08X,共 %d 字节" % (va, nbytes))
    disasm(va, nbytes, img, funcs, globs, stop_at_ret=(mode == "ret"))


main()
