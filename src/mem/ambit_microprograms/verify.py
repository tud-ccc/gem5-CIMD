#!/usr/bin/env python3
"""
Functionally verify Ambit microprograms and check that gem5-CIMD can parse
them (see README.md).

The program is simulated bit-parallel: every row is a W-bit integer, one bit
per DRAM column, so W random input pairs are tested at once. Inputs follow
README.md: operand after operand, LSB first (a on I0..I(n-1), b on
I(n)..I(2n-1), ...), result bits LSB first on O0.. . Semantics:
  AAP src dst   copy src to dst; a 3-row src group is a triple-row
                activation (all members become MAJ) whose result is copied
  AP [x, y, z]  triple-row activation, all members become MAJ(x, y, z)
  ~DCCx         reads/writes the negation of DCCx; C0/C1 are constant rows
usage: verify.py <op> <n> <program.txt> [...]   e.g. verify.py ge 32 ge32.txt
"""

import random
import re
import sys

W = 512
MASK = (1 << W) - 1

# Row tokens and bracket groups accepted by gem5-CIMD's parseRowToken() and
# parseBracketGroup() (src/mem/dram_interface.cc), keys sorted as there.
GEM5_SINGLE = {"T0", "T1", "T2", "T3", "DCC0", "~DCC0", "DCC1", "~DCC1", "C0", "C1"}
GEM5_GROUPS = {"T0,~DCC0", "T1,~DCC1", "T2,T3", "T0,T3", "T0,T1,T2",
               "T1,T2,T3", "DCC0,T1,T2", "DCC1,T0,T3"}


def is_data_row(tok):
    return len(tok) >= 2 and tok[0] in "IOS"


def gem5_ok(operand):
    if operand.startswith("["):
        toks = [t.strip() for t in operand[1:-1].split(",")]
        return ",".join(sorted(toks)) in GEM5_GROUPS or all(map(is_data_row, toks))
    return operand in GEM5_SINGLE or is_data_row(operand)


ARITY = {"abs": 1, "bitcount": 1, "ifelse": 3}


def reference(op, n, a, b=0, c=0):
    m = (1 << n) - 1
    if op == "sub":
        return (a - b) & m, n
    if op == "mul":
        return a * b, 2 * n
    if op == "div":
        return a // b, n
    if op == "eq":
        return int(a == b), 1
    if op == "gt":
        return int(a > b), 1
    if op == "ge":
        return int(a >= b), 1
    if op == "add":
        return (a + b) & m, n
    if op == "min":
        return min(a, b), n
    if op == "max":
        return max(a, b), n
    if op == "abs":
        return (-a & m if a >> (n - 1) else a), n
    if op == "bitcount":
        return bin(a).count("1"), n.bit_length()
    if op == "ifelse":  # a = mask, b = then-value, c = else-value (per bit)
        return (a & b) | (~a & m & c), n
    raise ValueError(op)


class Sim:
    def __init__(self):
        self.rows = {}

    def read(self, tok):
        if tok == "C0":
            return 0
        if tok == "C1":
            return MASK
        if tok.startswith("~"):
            return ~self.read(tok[1:]) & MASK
        if tok not in self.rows:
            raise ValueError(f"read of uninitialised row {tok}")
        return self.rows[tok]

    def write(self, tok, val):
        if tok in ("C0", "C1"):
            raise ValueError("write to constant row")
        if tok.startswith("~"):
            self.rows[tok[1:]] = ~val & MASK
        else:
            self.rows[tok] = val

    def activate(self, operand):
        """Value seen on the bitlines when `operand` is activated as source."""
        if not operand.startswith("["):
            return self.read(operand)
        toks = [t.strip() for t in operand[1:-1].split(",")]
        if len(toks) != 3:
            raise ValueError(f"source group must be a TRA: {operand}")
        x, y, z = (self.read(t) for t in toks)
        maj = (x & y) | (y & z) | (x & z)
        for t in toks:
            self.write(t, maj)
        return maj

    def store(self, operand, val):
        toks = [t.strip() for t in operand[1:-1].split(",")] if operand.startswith("[") else [operand]
        for t in toks:
            self.write(t, val)


OPERAND = r"(\[[^\]]*\]|\S+)"


def run(program, inputs):
    sim = Sim()
    sim.rows.update(inputs)
    for line in program:
        if m := re.fullmatch(rf"AAP\s+{OPERAND}\s+{OPERAND}", line):
            sim.store(m.group(2), sim.activate(m.group(1)))
        elif m := re.fullmatch(rf"AP\s+(\[[^\]]*\])", line):
            sim.activate(m.group(1))
        else:
            raise ValueError(f"cannot parse: {line}")
    return sim


def verify(op, n, path, rounds=4):
    program = [l.strip() for l in open(path) if l.strip()]
    if not program:
        return "EMPTY"
    bad = sorted({o for l in program for o in re.findall(OPERAND, l)[1:] if not gem5_ok(o)})
    for _ in range(rounds):
        k = ARITY.get(op, 2)
        xs = [[random.getrandbits(n) for _ in range(W)] for _ in range(k)]
        if op == "div":
            xs[1] = [v or 1 for v in xs[1]]
        if k >= 2:  # force some equal pairs so eq/ge/min/max see ties
            for i in range(0, W, 7):
                if op != "div" or xs[0][i]:
                    xs[1][i] = xs[0][i]
        rows = {}
        for i in range(n):
            for j in range(k):
                row = j * n + i
                rows[f"I{row}"] = sum(((xs[j][c] >> i) & 1) << c for c in range(W))
        sim = run(program, rows)
        for c in range(W):
            args = [x[c] for x in xs]
            want, width = reference(op, n, *args)
            got = sum(((sim.read(f"O{j}") >> c) & 1) << j for j in range(width))
            if got != want:
                return f"WRONG inputs={args} got={got} want={want}"
    return f"OK ({len(program)} instr)" + (f"  gem5-UNPARSEABLE {bad}" if bad else "")


if __name__ == "__main__":
    op, n = sys.argv[1], int(sys.argv[2])
    for path in sys.argv[3:]:
        try:
            res = verify(op, n, path)
        except Exception as e:  # report and continue with the next file
            res = f"ERROR {e}"
        print(f"{path}: {res}")
