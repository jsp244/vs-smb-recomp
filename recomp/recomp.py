#!/usr/bin/env python3
"""
recomp.py - 6502 static recompiler for the VS. Super Mario Bros. native port.

Reads the user's own .nes file, discovers the code in the PRG ROM (recursive
descent from the interrupt vectors, optionally seeded with execution coverage
recorded by the headless harness), and emits C source
in which every 6502 instruction is a labelled statement inside a routine
function. The emitted code calls the shared hardware runtime for memory
and timing, so it behaves exactly like the interpreter and can be
cosimulated against it.

Nothing from the ROM is written anywhere but the --out directory, which
is ignored by git. Run with your own cartridge dump.

usage: recomp.py --rom FILE --out DIR [--coverage FILE ...] [--symbols FILE]
                 [--layout {auto,flat,mmc3}] [--prg-mode {0,1,auto}] [--max-routine N]

Layouts: "flat" is one 32 KiB unit at $8000-$FFFF (NROM-256, or the Vs.
System's mapper 99 with 32 KiB of PRG), "mmc3" is 8 KiB units with fixed and
swappable slots. "auto" picks flat for 32 KiB of PRG.
"""
import argparse
import collections
import hashlib
import os
import sys

# ---------------------------------------------------------------------------
# Opcode table: (mnemonic, mode, base cycles) for all 256 opcodes
# ---------------------------------------------------------------------------
_T = """
BRK imp 7|ORA izx 6|KIL imp 2|SLO izx 8|NOP zp 3|ORA zp 3|ASL zp 5|SLO zp 5|PHP imp 3|ORA imm 2|ASL acc 2|ANC imm 2|NOP abs 4|ORA abs 4|ASL abs 6|SLO abs 6
BPL rel 2|ORA izy 5|KIL imp 2|SLO izy 8|NOP zpx 4|ORA zpx 4|ASL zpx 6|SLO zpx 6|CLC imp 2|ORA aby 4|NOP imp 2|SLO aby 7|NOP abx 4|ORA abx 4|ASL abx 7|SLO abx 7
JSR abs 6|AND izx 6|KIL imp 2|RLA izx 8|BIT zp 3|AND zp 3|ROL zp 5|RLA zp 5|PLP imp 4|AND imm 2|ROL acc 2|ANC imm 2|BIT abs 4|AND abs 4|ROL abs 6|RLA abs 6
BMI rel 2|AND izy 5|KIL imp 2|RLA izy 8|NOP zpx 4|AND zpx 4|ROL zpx 6|RLA zpx 6|SEC imp 2|AND aby 4|NOP imp 2|RLA aby 7|NOP abx 4|AND abx 4|ROL abx 7|RLA abx 7
RTI imp 6|EOR izx 6|KIL imp 2|SRE izx 8|NOP zp 3|EOR zp 3|LSR zp 5|SRE zp 5|PHA imp 3|EOR imm 2|LSR acc 2|ALR imm 2|JMP abs 3|EOR abs 4|LSR abs 6|SRE abs 6
BVC rel 2|EOR izy 5|KIL imp 2|SRE izy 8|NOP zpx 4|EOR zpx 4|LSR zpx 6|SRE zpx 6|CLI imp 2|EOR aby 4|NOP imp 2|SRE aby 7|NOP abx 4|EOR abx 4|LSR abx 7|SRE abx 7
RTS imp 6|ADC izx 6|KIL imp 2|RRA izx 8|NOP zp 3|ADC zp 3|ROR zp 5|RRA zp 5|PLA imp 4|ADC imm 2|ROR acc 2|ARR imm 2|JMP ind 5|ADC abs 4|ROR abs 6|RRA abs 6
BVS rel 2|ADC izy 5|KIL imp 2|RRA izy 8|NOP zpx 4|ADC zpx 4|ROR zpx 6|RRA zpx 6|SEI imp 2|ADC aby 4|NOP imp 2|RRA aby 7|NOP abx 4|ADC abx 4|ROR abx 7|RRA abx 7
NOP imm 2|STA izx 6|NOP imm 2|SAX izx 6|STY zp 3|STA zp 3|STX zp 3|SAX zp 3|DEY imp 2|NOP imm 2|TXA imp 2|XAA imm 2|STY abs 4|STA abs 4|STX abs 4|SAX abs 4
BCC rel 2|STA izy 6|KIL imp 2|SHA izy 6|STY zpx 4|STA zpx 4|STX zpy 4|SAX zpy 4|TYA imp 2|STA aby 5|TXS imp 2|TAS aby 5|SHY abx 5|STA abx 5|SHX aby 5|SHA aby 5
LDY imm 2|LDA izx 6|LDX imm 2|LAX izx 6|LDY zp 3|LDA zp 3|LDX zp 3|LAX zp 3|TAY imp 2|LDA imm 2|TAX imp 2|LXA imm 2|LDY abs 4|LDA abs 4|LDX abs 4|LAX abs 4
BCS rel 2|LDA izy 5|KIL imp 2|LAX izy 5|LDY zpx 4|LDA zpx 4|LDX zpy 4|LAX zpy 4|CLV imp 2|LDA aby 4|TSX imp 2|LAS aby 4|LDY abx 4|LDA abx 4|LDX aby 4|LAX aby 4
CPY imm 2|CMP izx 6|NOP imm 2|DCP izx 8|CPY zp 3|CMP zp 3|DEC zp 5|DCP zp 5|INY imp 2|CMP imm 2|DEX imp 2|AXS imm 2|CPY abs 4|CMP abs 4|DEC abs 6|DCP abs 6
BNE rel 2|CMP izy 5|KIL imp 2|DCP izy 8|NOP zpx 4|CMP zpx 4|DEC zpx 6|DCP zpx 6|CLD imp 2|CMP aby 4|NOP imp 2|DCP aby 7|NOP abx 4|CMP abx 4|DEC abx 7|DCP abx 7
CPX imm 2|SBC izx 6|NOP imm 2|ISC izx 8|CPX zp 3|SBC zp 3|INC zp 5|ISC zp 5|INX imp 2|SBC imm 2|NOP imp 2|SBC imm 2|CPX abs 4|SBC abs 4|INC abs 6|ISC abs 6
BEQ rel 2|SBC izy 5|KIL imp 2|ISC izy 8|NOP zpx 4|SBC zpx 4|INC zpx 6|ISC zpx 6|SED imp 2|SBC aby 4|NOP imp 2|ISC aby 7|NOP abx 4|SBC abx 4|INC abx 7|ISC abx 7
"""
OPCODES = []
for _row in _T.strip().splitlines():
    for _cell in _row.split("|"):
        _m, _mode, _c = _cell.split()
        OPCODES.append((_m, _mode, int(_c)))
assert len(OPCODES) == 256

OFFICIAL = {"ADC", "AND", "ASL", "BCC", "BCS", "BEQ", "BIT", "BMI", "BNE", "BPL", "BRK", "BVC", "BVS", "CLC",
            "CLD", "CLI", "CLV", "CMP", "CPX", "CPY", "DEC", "DEX", "DEY", "EOR", "INC", "INX", "INY", "JMP",
            "JSR", "LDA", "LDX", "LDY", "LSR", "NOP", "ORA", "PHA", "PHP", "PLA", "PLP", "ROL", "ROR", "RTI",
            "RTS", "SBC", "SEC", "SED", "SEI", "STA", "STX", "STY", "TAX", "TAY", "TSX", "TXA", "TXS", "TYA"}


def is_official(op):
    mn = OPCODES[op][0]
    return mn in OFFICIAL and (mn != "NOP" or op == 0xEA) and op != 0xEB


MODE_LEN = {"imp": 1, "acc": 1, "imm": 2, "zp": 2, "zpx": 2, "zpy": 2, "izx": 2, "izy": 2, "rel": 2,
            "abs": 3, "abx": 3, "aby": 3, "ind": 3}

BRANCHES = {"BPL": "!cpu.n", "BMI": "cpu.n", "BVC": "!cpu.v", "BVS": "cpu.v",
            "BCC": "!cpu.c", "BCS": "cpu.c", "BNE": "!cpu.z", "BEQ": "cpu.z"}
LOADS = {"LDA": "A", "LDX": "X", "LDY": "Y"}
ALU_A = {"ORA": "A |= {m}; set_nz(A);", "AND": "A &= {m}; set_nz(A);", "EOR": "A ^= {m}; set_nz(A);",
         "ADC": "op_adc({m});", "SBC": "op_sbc({m});", "CMP": "op_cmp(A, {m});",
         "CPX": "op_cmp(X, {m});", "CPY": "op_cmp(Y, {m});", "BIT": "op_bit({m});",
         "LAX": "A = X = {m}; set_nz(A);", "LAS": "op_las({m});",
         "ANC": "op_anc({m});", "ALR": "op_alr({m});", "ARR": "op_arr({m});", "AXS": "op_axs({m});",
         "XAA": "op_xaa({m});", "LXA": "op_lxa({m});"}
RMW_FN = {"ASL": "op_asl", "LSR": "op_lsr", "ROL": "op_rol", "ROR": "op_ror", "INC": "op_inc", "DEC": "op_dec",
          "SLO": "fn_slo", "RLA": "fn_rla", "SRE": "fn_sre", "RRA": "fn_rra", "DCP": "fn_dcp", "ISC": "fn_isc"}
STORES = {"STA": "A", "STX": "X", "STY": "Y", "SAX": "(A & X)"}
IMPLIED = {
    "CLC": "cpu.c = 0;", "SEC": "cpu.c = 1;", "CLV": "cpu.v = 0;", "CLD": "cpu.d = 0;", "SED": "cpu.d = 1;",
    "CLI": "cpu.irq_delay_i = cpu.i; cpu.irq_delay_active = 1; cpu.i = 0;",
    "SEI": "cpu.irq_delay_i = cpu.i; cpu.irq_delay_active = 1; cpu.i = 1;",
    "TAX": "X = A; set_nz(X);", "TAY": "Y = A; set_nz(Y);", "TXA": "A = X; set_nz(A);", "TYA": "A = Y; set_nz(A);",
    "TSX": "X = SP; set_nz(X);", "TXS": "SP = X;",
    "INX": "X++; set_nz(X);", "DEX": "X--; set_nz(X);", "INY": "Y++; set_nz(Y);", "DEY": "Y--; set_nz(Y);",
    "PHA": "push8(A);", "PHP": "push8(cpu_get_p(1));", "PLA": "A = pop8(); set_nz(A);",
    "PLP": "{ uint8_t old_i = cpu.i; cpu_set_p(pop8()); cpu.irq_delay_i = old_i; cpu.irq_delay_active = 1; }",
    "NOP": "",
}


class Insn:
    __slots__ = ("addr", "op", "mn", "mode", "length", "cycles", "operand", "raw", "target")

    def __init__(self, addr, op, mn, mode, length, cycles, operand, raw):
        self.addr, self.op, self.mn, self.mode, self.length, self.cycles = addr, op, mn, mode, length, cycles
        self.operand, self.raw = operand, raw
        self.target = None
        if mode == "rel":
            self.target = (addr + 2 + ((operand ^ 0x80) - 0x80)) & 0xFFFF
        elif mode == "abs" and mn in ("JMP", "JSR"):
            self.target = operand

    @property
    def next(self):
        return (self.addr + self.length) & 0xFFFF

    def falls_through(self):
        return self.mn not in ("JMP", "RTS", "RTI", "KIL")

    def text(self):
        v, m = self.operand, self.mode
        if m == "rel":
            f = f"${self.target:04X}"
        else:
            f = {"imp": "", "acc": "A", "imm": f"#${v:02X}", "zp": f"${v:02X}", "zpx": f"${v:02X},X",
                 "zpy": f"${v:02X},Y", "izx": f"(${v:02X},X)", "izy": f"(${v:02X}),Y", "abs": f"${v:04X}",
                 "abx": f"${v:04X},X", "aby": f"${v:04X},Y", "ind": f"(${v:04X})"}[m]
        return f"{self.mn} {f}".strip()


class Unit:
    """Code of one PRG bank (8 KiB, or all 32 KiB in the flat layout) translated for one CPU base address."""

    def __init__(self, bank, base, data):
        self.bank, self.base, self.data = bank, base, data
        self.end = base + len(data)
        self.insns = {}
        self.roots = set()
        self.worklist = []
        self.xrefs = collections.defaultdict(set)   # target -> set of source addrs
        self.comp = {}      # addr -> component id
        self.comps = []     # list of sorted address lists

    @property
    def name(self):
        if len(self.data) > 0x2000:
            return f"prg_{self.base:04X}"
        return f"b{self.bank:02d}_{self.base:04X}"

    def contains(self, a):
        return self.base <= a < self.end

    def decode(self, a):
        off = a - self.base
        op = self.data[off]
        mn, mode, cyc = OPCODES[op]
        ln = MODE_LEN[mode]
        if off + ln > len(self.data):
            return None
        raw = self.data[off:off + ln]
        operand = 0
        if ln == 2:
            operand = raw[1]
        elif ln == 3:
            operand = raw[1] | (raw[2] << 8)
        return Insn(a, op, mn, mode, ln, cyc, operand, bytes(raw))


class Recompiler:
    def __init__(self, prg, prg_mode, max_routine, symbols, flat=False, hooks=()):
        self.prg = prg
        self.flat = flat
        self.nbanks = 1 if flat else len(prg) // 0x2000
        self.prg_mode = prg_mode
        self.max_routine = max_routine
        self.symbols = symbols
        self.hooks = set(hooks)    # instructions the runtime looks at before they run (HOOK in recomp_rt.h)
        self.units = {}            # (bank, base) -> Unit
        self.ext_refs = collections.defaultdict(set)  # slot base -> set of target addrs (swappable slots)
        self.ext_pending = collections.defaultdict(set)
        # routines labelled JumpEngine* in the symbols file pop their return address and jump
        # through the table of words that follows the JSR (the entry is chosen by A)
        self.jump_engines = {a for a, n in symbols.items() if n.startswith("JumpEngine")}

    # ---- bank layout ----
    def fixed_layout(self):
        if self.flat:
            return {0x8000: 0}, ()
        last, second = self.nbanks - 1, self.nbanks - 2
        if self.prg_mode == 0:
            return {0xC000: second, 0xE000: last}, (0x8000, 0xA000)
        return {0x8000: second, 0xE000: last}, (0xA000, 0xC000)

    def unit(self, bank, base):
        key = (bank, base)
        if key not in self.units:
            if self.flat:
                self.units[key] = Unit(bank, base, self.prg)
            else:
                self.units[key] = Unit(bank, base, self.prg[bank * 0x2000:(bank + 1) * 0x2000])
        return self.units[key]

    def unit_at(self, addr):
        """fixed unit covering addr, or None if the slot is swappable"""
        if self.flat:
            return self.unit(0, 0x8000) if addr >= 0x8000 else None
        fixed, _ = self.fixed_layout()
        base = addr & 0xE000
        if base in fixed:
            return self.unit(fixed[base], base)
        return None

    # ---- analysis ----
    def add_root(self, unit, a, src=None):
        if not unit.contains(a):
            return
        if src is not None:
            unit.xrefs[a].add(src)
        if a not in unit.insns:
            unit.worklist.append(a)
        unit.roots.add(a)

    def add_target(self, src_unit, src, target, is_call):
        """control transfer from src (in src_unit) to target anywhere in the address space"""
        if src_unit.contains(target):
            src_unit.xrefs[target].add(src)
            if target not in src_unit.insns:
                src_unit.worklist.append(target)
            if is_call:
                src_unit.roots.add(target)
            return
        if target < 0x8000:
            return
        u = self.unit_at(target)
        if u is not None:
            self.add_root(u, target, src)
        else:
            base = target & 0xE000
            if target not in self.ext_refs[base]:
                self.ext_refs[base].add(target)
                self.ext_pending[base].add(target)

    def add_jump_table(self, unit, a):
        """the words at a are code pointers; the table ends at the first word that does not
        point at an official instruction in the fixed PRG (a wrong guess only adds a routine
        that is never entered)"""
        for k in range(128):
            p = a + 2 * k
            if not unit.contains(p + 1):
                break
            t = unit.data[p - unit.base] | (unit.data[p + 1 - unit.base] << 8)
            u = self.unit_at(t)
            if u is None or not is_official(u.data[t - u.base]):
                break
            self.add_root(u, t, p)

    def descend(self, unit):
        changed = False
        while unit.worklist:
            a = unit.worklist.pop()
            if a in unit.insns or not unit.contains(a):
                continue
            ins = unit.decode(a)
            if ins is None:
                continue
            unit.insns[a] = ins
            changed = True
            if ins.mn in BRANCHES:
                self.add_target(unit, a, ins.target, False)
                unit.worklist.append(ins.next)
            elif ins.mn == "JMP" and ins.mode == "abs":
                self.add_target(unit, a, ins.target, False)
            elif ins.mn == "JSR":
                self.add_target(unit, a, ins.target, True)
                if ins.target in self.jump_engines:
                    self.add_jump_table(unit, ins.next)
                else:
                    unit.worklist.append(ins.next)
            elif ins.mn == "BRK":
                unit.worklist.append(ins.next)
            elif ins.falls_through():
                unit.worklist.append(ins.next)
        return changed

    def analyse(self, coverage, bank_bases):
        fixed, swappable = self.fixed_layout()
        # fixed banks: vectors
        last_unit = self.unit(0, 0x8000) if self.flat else self.unit(self.nbanks - 1, 0xE000)
        for vec in (0xFFFA, 0xFFFC, 0xFFFE):
            off = vec - last_unit.base
            target = last_unit.data[off] | (last_unit.data[off + 1] << 8)
            u = self.unit_at(target)
            if u is not None:
                self.add_root(u, target)
            else:
                self.ext_refs[target & 0xE000].add(target)
                self.ext_pending[target & 0xE000].add(target)
        for base, bank in fixed.items():
            self.unit(bank, base)
        # swappable banks: one unit per observed/inferred base
        for bank in range(self.nbanks):
            if bank in fixed.values():
                continue
            for base in bank_bases.get(bank, ()):
                self.unit(bank, base)
        # coverage roots (a byte per PRG byte, bit k = executed while mapped in slot k)
        if coverage is not None:
            for (bank, base), u in self.units.items():
                for off in range(len(u.data)):
                    slot = ((base + off) >> 13) & 3
                    if coverage[bank * 0x2000 + off] & (1 << slot):
                        self.add_root(u, base + off)
        # iterate to a fixpoint: external references seed every unit that lives at that base
        while True:
            changed = False
            for u in list(self.units.values()):
                if self.descend(u):
                    changed = True
            for base in list(self.ext_pending.keys()):
                targets = self.ext_pending[base]
                if not targets:
                    continue
                self.ext_pending[base] = set()
                for u in self.units.values():
                    if u.base == base:
                        for t in targets:
                            self.add_root(u, t)
                        changed = True
            if not changed:
                break

    # ---- routine partition ----
    def partition(self, unit):
        addrs = sorted(unit.insns)
        parent = {a: a for a in addrs}
        size = {a: 1 for a in addrs}

        def find(a):
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a

        def union(a, b):
            ra, rb = find(a), find(b)
            if ra == rb:
                return True
            if size[ra] + size[rb] > self.max_routine:
                return False
            if size[ra] < size[rb]:
                ra, rb = rb, ra
            parent[rb] = ra
            size[ra] += size[rb]
            return True

        # fallthrough and branch edges first (keep loops together), then JMPs
        edges_first, edges_jmp = [], []
        for a, ins in unit.insns.items():
            if ins.mn in BRANCHES:
                if ins.target in unit.insns:
                    edges_first.append((a, ins.target))
                if ins.next in unit.insns:
                    edges_first.append((a, ins.next))
            elif ins.mn == "JMP" and ins.mode == "abs":
                if ins.target in unit.insns:
                    edges_jmp.append((a, ins.target))
            elif ins.falls_through() and ins.mn != "JSR":
                if ins.next in unit.insns:
                    edges_first.append((a, ins.next))
            elif ins.mn == "JSR" and ins.next in unit.insns:
                edges_first.append((a, ins.next))
        for a, b in edges_first:
            union(a, b)
        for a, b in edges_jmp:
            union(a, b)
        groups = collections.defaultdict(list)
        for a in addrs:
            groups[find(a)].append(a)
        unit.comps = sorted(groups.values(), key=lambda g: g[0])
        unit.comp = {}
        for i, g in enumerate(unit.comps):
            for a in g:
                unit.comp[a] = i

    # ---- emission ----
    def read_expr(self, ins):
        v, m = ins.operand, ins.mode
        if m == "imm":
            return f"0x{v:02X}"
        if m == "zp":
            return f"ram[0x{v:02X}]"
        if m == "zpx":
            return f"ram[(uint8_t)(0x{v:02X} + X)]"
        if m == "zpy":
            return f"ram[(uint8_t)(0x{v:02X} + Y)]"
        if m == "abs":
            if v < 0x2000:
                return f"ram[0x{v & 0x7FF:03X}]"
            return f"mem_read(0x{v:04X})"
        if m in ("abx", "aby"):
            idx = "X" if m == "abx" else "Y"
            if v + 255 < 0x2000:
                return f"rd_idx_ram(0x{v:04X}, {idx})"
            return f"rd_idx(0x{v:04X}, {idx})"
        if m == "izx":
            return f"mem_read(zp_read16((uint8_t)(0x{v:02X} + X)))"
        if m == "izy":
            return f"rd_idx(zp_read16(0x{v:02X}), Y)"
        raise ValueError(m)

    def write_stmt(self, ins, val, nxt):
        """statements performing a store; includes the bank-switch exit when ROM may be written
        (not in the flat layout, where PRG never changes)"""
        v, m = ins.operand, ins.mode
        ex = "" if self.flat else f"EXIT(0x{nxt:04X});"
        if m == "zp":
            return f"ram[0x{v:02X}] = {val};"
        if m == "zpx":
            return f"ram[(uint8_t)(0x{v:02X} + X)] = {val};"
        if m == "zpy":
            return f"ram[(uint8_t)(0x{v:02X} + Y)] = {val};"
        if m == "abs":
            if v < 0x2000:
                return f"ram[0x{v & 0x7FF:03X}] = {val};"
            s = f"mem_write(0x{v:04X}, {val});"
            return s + (" " + ex if v >= 0x8000 and ex else "")
        if m in ("abx", "aby"):
            idx = "X" if m == "abx" else "Y"
            if v + 255 < 0x2000:
                return f"ram[(uint16_t)(0x{v:04X} + {idx}) & 0x7FF] = {val};"
            return f"{{ uint16_t a_ = addr_idx_wr(0x{v:04X}, {idx}); mem_write(a_, {val});{self.rom_exit(ex)} }}"
        if m == "izx":
            return f"{{ uint16_t a_ = zp_read16((uint8_t)(0x{v:02X} + X)); mem_write(a_, {val});{self.rom_exit(ex)} }}"
        if m == "izy":
            return f"{{ uint16_t a_ = addr_idx_wr(zp_read16(0x{v:02X}), Y); mem_write(a_, {val});{self.rom_exit(ex)} }}"
        raise ValueError(m)

    def rmw_stmt(self, ins, fn, nxt):
        v, m = ins.operand, ins.mode
        ex = "" if self.flat else f"EXIT(0x{nxt:04X});"
        if m == "acc":
            return f"A = {fn}(A);"
        if m == "zp":
            return f"ram[0x{v:02X}] = {fn}(ram[0x{v:02X}]);"
        if m in ("zpx", "zpy"):
            idx = "X" if m == "zpx" else "Y"
            return f"{{ uint8_t a_ = (uint8_t)(0x{v:02X} + {idx}); ram[a_] = {fn}(ram[a_]); }}"
        if m == "abs":
            if v < 0x2000:
                return f"ram[0x{v & 0x7FF:03X}] = {fn}(ram[0x{v & 0x7FF:03X}]);"
            return f"RMW_AT(0x{v:04X}, {fn});" + (" " + ex if v >= 0x8000 and ex else "")
        if m in ("abx", "aby"):
            idx = "X" if m == "abx" else "Y"
            if v + 255 < 0x2000:
                return f"{{ uint16_t a_ = (uint16_t)(0x{v:04X} + {idx}) & 0x7FF; ram[a_] = {fn}(ram[a_]); }}"
            return f"{{ uint16_t a_ = addr_idx_wr(0x{v:04X}, {idx}); RMW_AT(a_, {fn});{self.rom_exit(ex)} }}"
        if m == "izx":
            return f"{{ uint16_t a_ = zp_read16((uint8_t)(0x{v:02X} + X)); RMW_AT(a_, {fn});{self.rom_exit(ex)} }}"
        if m == "izy":
            return f"{{ uint16_t a_ = addr_idx_wr(zp_read16(0x{v:02X}), Y); RMW_AT(a_, {fn});{self.rom_exit(ex)} }}"
        raise ValueError(m)

    @staticmethod
    def rom_exit(ex):
        return f" if (a_ >= 0x8000) {ex}" if ex else ""

    def emit_insn(self, unit, ins, comp_set, out):
        a, mn, nxt = ins.addr, ins.mn, ins.next
        sym = self.symbols.get(a)
        hexb = " ".join(f"{b:02X}" for b in ins.raw)
        label = f" <{sym}>" if sym else ""
        hook = f" HOOK(0x{a:04X});" if a in self.hooks else ""
        out.append(f"    case 0x{a:04X}: L_{a:04X}: INSN(0x{a:04X});{hook} /* {hexb:<8} {ins.text()}{label} */")
        body = []
        cyc = f"CYC({ins.cycles});"
        sync = f"SYNC(0x{nxt:04X});"

        if mn in BRANCHES:
            cond = BRANCHES[mn]
            t = ins.target
            cross = 1 if ((t ^ nxt) & 0x100) else 0
            if t in comp_set:
                taken = f"cpu.cycles += {1 + cross}; SYNC(0x{t:04X}); goto L_{t:04X};"
            else:
                taken = f"cpu.cycles += {1 + cross}; EXIT(0x{t:04X});"
            body.append(f"{cyc} if ({cond}) {{ {taken} }} {sync}")
        elif mn in LOADS:
            r = LOADS[mn]
            body.append(f"{cyc} {r} = {self.read_expr(ins)}; set_nz({r}); {sync}")
        elif mn in ALU_A:
            body.append(f"{cyc} {ALU_A[mn].format(m=self.read_expr(ins))} {sync}")
        elif mn in STORES:
            body.append(f"{cyc} {self.write_stmt(ins, STORES[mn], nxt)} {sync}")
        elif mn in RMW_FN:
            body.append(f"{cyc} {self.rmw_stmt(ins, RMW_FN[mn], nxt)} {sync}")
        elif mn in IMPLIED:
            if mn == "NOP" and ins.mode not in ("imp", "imm"):
                body.append(f"{cyc} (void){self.read_expr(ins)}; {sync}")
            elif mn == "NOP":
                body.append(f"{cyc} {sync}")
            else:
                body.append(f"{cyc} {IMPLIED[mn]} {sync}")
        elif mn == "JMP":
            if ins.mode == "abs":
                t = ins.target
                if t in comp_set:
                    body.append(f"{cyc} SYNC(0x{t:04X}); goto L_{t:04X};")
                else:
                    body.append(f"{cyc} EXIT(0x{t:04X});")
            else:
                body.append(f"{cyc} {{ uint16_t t_ = jmp_ind(0x{ins.operand:04X}); EXIT(t_); }}")
        elif mn == "JSR":
            body.append(f"{cyc} push16(0x{(nxt - 1) & 0xFFFF:04X}); EXIT(0x{ins.target:04X});")
        elif mn == "RTS":
            body.append(f"{cyc} {{ uint16_t t_ = (uint16_t)(pop16() + 1); EXIT(t_); }}")
        elif mn == "RTI":
            body.append(f"{cyc} cpu_set_p(pop8()); {{ uint16_t t_ = pop16(); EXIT(t_); }}")
        elif mn == "BRK":
            body.append(f"{cyc} push16(0x{(a + 2) & 0xFFFF:04X}); push8(cpu_get_p(1)); cpu.i = 1; "
                        f"{{ uint16_t t_ = mem_read16(cpu.nmi_pending ? 0xFFFA : 0xFFFE); cpu.nmi_pending = 0; EXIT(t_); }}")
        elif mn == "KIL":
            body.append(f"{cyc} cpu.jammed = 1; EXIT(0x{a:04X});")
        elif mn in ("SHY", "SHX", "SHA", "TAS"):
            v = ins.operand
            if ins.mode == "izy":
                base = f"zp_read16(0x{v:02X})"
            else:
                base = f"0x{v:04X}"
            idx = "X" if ins.mode == "abx" else "Y"
            val = {"SHY": "Y", "SHX": "X", "SHA": "(A & X)", "TAS": "SP"}[mn]
            pre = "SP = A & X; " if mn == "TAS" else ""
            body.append(f"{cyc} {pre}op_unstable_store({base}, {idx}, {val}); EXIT(0x{nxt:04X});")
        else:
            raise ValueError(f"unhandled mnemonic {mn}")
        out.append("        " + " ".join(body))
        return nxt

    def emit_unit(self, unit, outdir):
        self.partition(unit)
        lines = []
        what = "PRG ROM" if self.flat else f"PRG bank {unit.bank}"
        lines.append(f"/* {unit.name}: {what} translated for ${unit.base:04X}-${unit.end - 1:04X}.")
        lines.append(" * Generated by recomp/recomp.py from the user's ROM - do not commit. */")
        lines.append('#include "recomp_rt.h"')
        lines.append("")
        func_names = []
        for ci, comp in enumerate(unit.comps):
            root = comp[0]
            sym = self.symbols.get(root)
            fname = f"r_{unit.name}_{root:04X}" + (f"_{sym}" if sym else "")
            func_names.append(fname)
            comp_set = set(comp)
            lines.append(f"/* routine at ${root:04X}, {len(comp)} instructions */")
            lines.append(f"static void {fname}(void) {{")
            lines.append("    switch (cpu.pc) {")
            out = []
            for i, a in enumerate(comp):
                ins = unit.insns[a]
                nxt = self.emit_insn(unit, ins, comp_set, out)
                if ins.falls_through() and ins.mn != "JSR":
                    following = comp[i + 1] if i + 1 < len(comp) else None
                    if nxt != following:
                        if nxt in comp_set:
                            out.append(f"        goto L_{nxt:04X};")
                        else:
                            out.append(f"        EXIT(0x{nxt:04X});")
            lines.extend(out)
            lines.append("    default: return;")
            lines.append("    }")
            lines.append("}")
            lines.append("")
        # entry table
        lines.append(f"static const uint16_t entry_{unit.name}[0x{len(unit.data):04X}] = {{")
        row = []
        for off in range(len(unit.data)):
            a = unit.base + off
            idx = unit.comp.get(a, -1) + 1
            row.append(str(idx))
            if len(row) == 32:
                lines.append("    " + ",".join(row) + ",")
                row = []
        lines.append("};")
        lines.append(f"static const recomp_fn funcs_{unit.name}[] = {{")
        for f in func_names:
            lines.append(f"    {f},")
        lines.append("};")
        lines.append(f"const recomp_unit_t unit_{unit.name} = {{ entry_{unit.name}, funcs_{unit.name} }};")
        with open(os.path.join(outdir, f"{unit.name}.c"), "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        return func_names

    def emit_main(self, outdir, sha1):
        lines = ['#include "recomp_rt.h"', "", f'const char *recomp_rom_sha1 = "{sha1}";', ""]
        for u in self.units.values():
            lines.append(f"extern const recomp_unit_t unit_{u.name};")
        lines.append("")
        if self.flat:
            # one unit for the whole fixed 32 KiB: no bank lookup
            name = self.unit(0, 0x8000).name
            lines.append("int recomp_run(void) {")
            lines.append("    uint16_t pc = cpu.pc;")
            lines.append("    if (pc < 0x8000 || cpu.jammed) return 0;")
            lines.append(f"    uint16_t idx = unit_{name}.entry[pc - 0x8000];")
            lines.append("    if (!idx) return 0;")
            lines.append("    sys.native_blocks++;")
            lines.append(f"    unit_{name}.funcs[idx - 1]();")
            lines.append("    return 1;")
            lines.append("}")
            with open(os.path.join(outdir, "recomp_main.c"), "w", newline="\n") as f:
                f.write("\n".join(lines) + "\n")
            return
        lines.append(f"static const recomp_unit_t *recomp_units[{self.nbanks}][4] = {{")
        for bank in range(self.nbanks):
            cells = []
            for slot in range(4):
                base = 0x8000 + slot * 0x2000
                u = self.units.get((bank, base))
                cells.append(f"&unit_{u.name}" if u else "NULL")
            lines.append("    { " + ", ".join(cells) + " },")
        lines.append("};")
        lines.append("")
        lines.append("int recomp_run(void) {")
        lines.append("    uint16_t pc = cpu.pc;")
        lines.append("    if (pc < 0x8000 || cpu.jammed) return 0;")
        lines.append("    int slot = (pc >> 13) & 3;")
        lines.append("    const recomp_unit_t *u = recomp_units[mapper.prg_bank[slot]][slot];")
        lines.append("    if (!u) return 0;")
        lines.append("    uint16_t idx = u->entry[pc & 0x1FFF];")
        lines.append("    if (!idx) return 0;")
        lines.append("    sys.native_blocks++;")
        lines.append("    u->funcs[idx - 1]();")
        lines.append("    return 1;")
        lines.append("}")
        with open(os.path.join(outdir, "recomp_main.c"), "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")

    def emit_listing(self, unit, outdir):
        os.makedirs(os.path.join(outdir, "listing"), exist_ok=True)
        with open(os.path.join(outdir, "listing", f"{unit.name}.lst"), "w", newline="\n") as f:
            f.write(f"; PRG bank {unit.bank} at ${unit.base:04X}  ({len(unit.insns)} instructions, {len(unit.comps)} routines)\n")
            for a in sorted(unit.insns):
                ins = unit.insns[a]
                sym = self.symbols.get(a)
                if a in unit.roots or a in self.symbols:
                    f.write(f"\n{sym or f'sub_{a:04X}'}:\n")
                refs = unit.xrefs.get(a)
                xr = ("  ; <- " + ",".join(f"{s:04X}" for s in sorted(refs))) if refs else ""
                hexb = " ".join(f"{b:02X}" for b in ins.raw)
                f.write(f"  {a:04X}  {hexb:<9} {ins.text():<16}{xr}\n")


def load_coverage(paths, prg_size):
    cov = bytearray(prg_size)
    for p in paths:
        data = open(p, "rb").read()
        if len(data) < prg_size:
            raise SystemExit(f"coverage file {p} is too small for this ROM")
        for i in range(prg_size):
            cov[i] |= data[i]
    return cov


def infer_bases(prg, nbanks, fixed_banks, swappable, coverage):
    """which base address(es) each swappable bank is executed at"""
    bases = {}
    for bank in range(nbanks):
        if bank in fixed_banks:
            continue
        found = set()
        if coverage is not None:
            for slot_base in swappable:
                slot = (slot_base >> 13) & 3
                seg = coverage[bank * 0x2000:(bank + 1) * 0x2000]
                if any(b & (1 << slot) for b in seg):
                    found.add(slot_base)
        if not found:
            # heuristic: count JSR/JMP operands pointing into each swappable window
            data = prg[bank * 0x2000:(bank + 1) * 0x2000]
            votes = collections.Counter()
            for i in range(0x2000 - 2):
                if data[i] in (0x20, 0x4C):
                    t = data[i + 1] | (data[i + 2] << 8)
                    for slot_base in swappable:
                        if slot_base <= t < slot_base + 0x2000:
                            votes[slot_base] += 1
            if votes:
                found.add(votes.most_common(1)[0][0])
            else:
                found.add(swappable[0])
        bases[bank] = sorted(found)
    return bases


def detect_prg_mode(prg):
    """look for LDA #imm / STA $8000 in the fixed banks to find the MMC3 PRG mode bit"""
    last16 = prg[-0x4000:]
    votes = collections.Counter()
    for i in range(len(last16) - 4):
        if last16[i] == 0xA9 and last16[i + 2] == 0x8D and last16[i + 3] == 0x00 and last16[i + 4] == 0x80:
            votes[(last16[i + 1] >> 6) & 1] += 1
    if votes:
        return votes.most_common(1)[0][0]
    return 0


# MAME's "suprmrio" set: the chips by CRC32, in image order (PRG $8000-$FFFF, then CHR $0000-$1FFF)
MAME_CHIPS = (0xbe4d5436, 0x5e3fb550, 0xb1b87893, 0x1abf053c, 0x42418d40, 0x15506b86)


def mame_set_image(path):
    """the zip's chips as the iNES image a dump would be (NES 2.0 header: Vs. System, mapper 99)"""
    import zipfile
    with zipfile.ZipFile(path) as z:
        by_crc = {i.CRC: i for i in z.infolist() if i.file_size == 0x2000}
        missing = [f"{c:08x}" for c in MAME_CHIPS if c not in by_crc]
        if missing:
            raise SystemExit(f"{path}: not MAME's VS. Super Mario Bros. set (no chip with CRC {', '.join(missing)})")
        chips = [z.read(by_crc[c]) for c in MAME_CHIPS]
    return bytes([0x4E, 0x45, 0x53, 0x1A, 2, 2, 0x31, 0x69, 0, 0, 0, 0, 0, 5, 0, 0]) + b"".join(chips)


def load_symbols(path):
    """ADDR name [hook] lines -> ({addr: name}, {addrs marked hook})"""
    syms, hooks = {}, set()
    if not path or not os.path.exists(path):
        return syms, hooks
    for line in open(path):
        line = line.split(";")[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) >= 2:
            syms[int(parts[0], 16)] = parts[1]
            if "hook" in parts[2:]:
                hooks.add(int(parts[0], 16))
    return syms, hooks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--coverage", action="append", default=[])
    ap.add_argument("--symbols", default=os.path.join(os.path.dirname(__file__), "symbols.txt"))
    ap.add_argument("--layout", default="auto", choices=("auto", "flat", "mmc3"))
    ap.add_argument("--prg-mode", default="auto")
    ap.add_argument("--max-routine", type=int, default=1200)
    ap.add_argument("--listing", action="store_true", help="also write annotated disassembly listings")
    args = ap.parse_args()

    data = open(args.rom, "rb").read()
    if data[:4] == b"PK\x03\x04":
        data = mame_set_image(args.rom)
    if data[:4] != b"NES\x1a":
        raise SystemExit("not an iNES file")
    prg_units, chr_units = data[4], data[5]
    nes2 = (data[7] & 0x0C) == 0x08
    if nes2:
        prg_units |= (data[9] & 0x0F) << 8
        chr_units |= (data[9] & 0xF0) << 4
    off = 16 + (512 if data[6] & 4 else 0)
    prg_size = prg_units * 16384
    prg = data[off:off + prg_size]
    sha1 = hashlib.sha1(data[off:off + prg_size + chr_units * 8192]).hexdigest()
    nbanks = prg_size // 0x2000

    coverage = load_coverage(args.coverage, prg_size) if args.coverage else None
    flat = args.layout == "flat" or (args.layout == "auto" and prg_size == 0x8000)
    if flat and prg_size != 0x8000:
        raise SystemExit("the flat layout needs exactly 32 KiB of PRG ROM")
    if flat:
        prg_mode = 0
    elif args.prg_mode == "auto":
        prg_mode = detect_prg_mode(prg)
        if coverage is not None:
            # the second-to-last bank runs at $8000 in mode 1, at $C000 in mode 0
            seg = coverage[(nbanks - 2) * 0x2000:(nbanks - 1) * 0x2000]
            if any(b & 1 for b in seg):
                prg_mode = 1
            elif any(b & 4 for b in seg):
                prg_mode = 0
    else:
        prg_mode = int(args.prg_mode)

    symbols, hooks = load_symbols(args.symbols)
    rc = Recompiler(prg, prg_mode, args.max_routine, symbols, flat, hooks)
    fixed, swappable = rc.fixed_layout()
    bank_bases = {} if flat else infer_bases(prg, nbanks, set(fixed.values()), swappable, coverage)
    rc.analyse(coverage, bank_bases)

    os.makedirs(args.out, exist_ok=True)
    for f in os.listdir(args.out):
        if f.endswith(".c"):
            os.remove(os.path.join(args.out, f))
    total_insns = total_routines = 0
    for key in sorted(rc.units):
        u = rc.units[key]
        if not u.insns:
            continue
        funcs = rc.emit_unit(u, args.out)
        if args.listing:
            rc.emit_listing(u, args.out)
        total_insns += len(u.insns)
        total_routines += len(funcs)
        biggest = max(len(c) for c in u.comps)
        print(f"  {u.name}: {len(u.insns):5d} instructions, {len(funcs):4d} routines (largest {biggest})")
    rc.units = {k: v for k, v in rc.units.items() if v.insns}
    rc.emit_main(args.out, sha1)
    with open(os.path.join(args.out, ".gitignore"), "w", newline="\n") as f:
        f.write("*\n!.gitignore\n")
    layout = "flat 32 KiB" if flat else f"PRG mode {prg_mode}"
    print(f"{layout}; {len(rc.units)} units, {total_insns} instructions, {total_routines} routines")
    print(f"ROM sha1 {sha1}")


if __name__ == "__main__":
    main()
