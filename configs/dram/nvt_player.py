"""
nvt_player.py  --  replay an .nvt row-activation trace against a MemCtrl

Each line of the trace is one DRAM command:

  <seq> T <row> <data> 0          triple-row activate  -> ROWAP
  <seq> O <src> <data> 0 <dst>    overlapped activate  -> ROWAAP

The <data> column is the row content the generator expected; this replay is
timing-only, so it is ignored.  Addresses are dense multiples of
--trace-row-size and are mapped onto DRAM rows of a single bank (an AAP
copies through shared sense amplifiers, so source and destination must share
a bank and subarray, and the trace carries no bank information).

Usage:
  gem5.opt configs/dram/nvt_player.py --trace=tests/1024.nvt
  gem5.opt configs/dram/nvt_player.py --trace=... --mem-type=HBM3_4Gb_1x64
"""

import argparse
import math
import re
import sys

import m5
from m5.objects import *
from m5.util import addToPath, fatal

addToPath("../")

from common import ObjectList

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

parser = argparse.ArgumentParser(description=__doc__)

parser.add_argument(
    "--trace", default="", help="Path to the .nvt trace file (required)"
)
parser.add_argument(
    "--mem-type",
    default="DDR4_2400_8x8",
    choices=ObjectList.mem_list.get_names(),
    help="DRAM type (default: DDR4_2400_8x8)",
)
parser.add_argument(
    "--addr-map",
    default="",
    help="Override addr_mapping (e.g. RoRaBaCoCh, RoRaBaChCo). "
    "Empty = use the memory type's default.",
)
parser.add_argument(
    "--trace-row-size",
    type=int,
    default=1024,
    help="Byte granularity of addresses in the trace (default 1024)",
)
parser.add_argument(
    "--bank",
    type=int,
    default=0,
    help="Bank the trace is replayed into (default 0)",
)
parser.add_argument(
    "--no-compact-rows",
    action="store_true",
    help="Use raw address/trace-row-size indices instead of packing "
    "distinct addresses into dense rows (needs a much larger --rows)",
)
parser.add_argument(
    "--rows",
    type=int,
    default=512,
    help="Rows per bank to size the address space with (default 512 = one "
    "subarray). Must exceed the highest row index in the trace.",
)

# Pipelined send engine (TracePlayerBase)
parser.add_argument(
    "--issue-depth",
    type=int,
    default=0,
    help="Max outstanding requests (0=unbounded, 1=serial)",
)
parser.add_argument(
    "--issue-interval",
    type=int,
    default=0,
    help="Min ticks between issues (0=no throttle)",
)
parser.add_argument(
    "--write-buffer-size",
    type=int,
    default=0,
    help="Controller write queue entries (0 = the memory type's default). "
    "Row-ops are write packets, so this is the admission window.",
)
parser.add_argument(
    "--max-tick",
    type=int,
    default=0,
    help="Stop the simulation after this many ticks (0 = run to completion)",
)
parser.add_argument(
    "--membus-clock",
    default="2.0GHz",
    help="Membus/system clock (default 2.0GHz)",
)

args = parser.parse_args()

if not args.trace:
    print("Error: --trace is required")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Memory geometry
# ---------------------------------------------------------------------------

intf_cls = ObjectList.mem_list.get(args.mem_type)
tmp = intf_cls()

banks_per_channel = int(tmp.banks_per_rank.value) * int(
    tmp.ranks_per_channel.value
)

if args.bank < 0 or args.bank >= banks_per_channel:
    fatal(
        "--bank=%d is out of range for '%s', which has %d banks per channel"
        % (args.bank, args.mem_type, banks_per_channel)
    )

# Effective row buffer size per rank; the address stride between rows.
row_stride = int(tmp.device_rowbuffer_size.getValue()) * int(
    tmp.devices_per_rank.value
)

# Address space: --rows rows per local bank, rounded up to a power of 2.
mem_size_min = args.rows * banks_per_channel * row_stride
mem_size = 1 << int(math.ceil(math.log(mem_size_min, 2)))

base_addr = 0

# ---------------------------------------------------------------------------
# System + clock
# ---------------------------------------------------------------------------

system = System(membus=IOXBar(width=32))
system.clk_domain = SrcClockDomain(
    clock=args.membus_clock, voltage_domain=VoltageDomain(voltage="1V")
)

system.mem_ranges = [AddrRange(base_addr, size=mem_size)]

iface = intf_cls()
if args.addr_map:
    iface.addr_mapping = args.addr_map
iface.range = system.mem_ranges[0]
if args.write_buffer_size > 0:
    iface.write_buffer_size = args.write_buffer_size
ctrl = MemCtrl(dram=iface)
system.mem_ctrls = [ctrl]
system.mem_ctrls[0].port = system.membus.mem_side_ports

# ---------------------------------------------------------------------------
# NvtTracePlayer
# ---------------------------------------------------------------------------

system.player = NvtTracePlayer(
    trace_file=args.trace,
    base_addr=base_addr,
    trace_row_size=args.trace_row_size,
    banks_per_channel=banks_per_channel,
    row_stride=row_stride,
    bank=args.bank,
    compact_rows=not args.no_compact_rows,
    issue_depth=args.issue_depth,
    issue_interval=args.issue_interval,
)

system.player.port = system.membus.cpu_side_ports
system.system_port = system.membus.cpu_side_ports

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

root = Root(full_system=False, system=system)
root.system.mem_mode = "timing"

m5.instantiate()

addr_map_str = args.addr_map if args.addr_map else str(tmp.addr_mapping)
print()
print("=" * 64)
print("NVT row-activation trace player")
print("  Trace          :", args.trace)
print("  Memory type    :", args.mem_type)
print("  Banks/channel  :", banks_per_channel)
print("  Bank replayed  :", args.bank)
print("  Row stride     : %d B  (DRAM row buffer size)" % row_stride)
print("  Trace row size : %d B" % args.trace_row_size)
print("  Compact rows   :", not args.no_compact_rows)
print("  Memory size    : %d MB" % (mem_size >> 20))
print("  Addr map       :", addr_map_str)
print("=" * 64)
print()

exit_event = m5.simulate(args.max_tick if args.max_tick > 0 else m5.MaxTick)
print("Simulation exited:", exit_event.getCause(), "@ tick", m5.curTick())

m5.stats.dump()

# ---------------------------------------------------------------------------
# Stats summary
# ---------------------------------------------------------------------------

stats_path = m5.options.outdir + "/stats.txt"
try:
    with open(stats_path) as f:
        text = f.read()

    def extract(pat, txt):
        m = re.search(pat, txt)
        return m.group(1) if m else "N/A"

    ap = extract(r"system\.player\.numAp\s+(\S+)", text)
    aap = extract(r"system\.player\.numAap\s+(\S+)", text)
    retries = extract(r"system\.player\.numRetries\s+(\S+)", text)
    maj3 = extract(r"system\.mem_ctrls\.dram\.nrMaj3\s+(\S+)", text)
    clones = extract(r"system\.mem_ctrls\.dram\.nrRowClones\s+(\S+)", text)
    final_tick = extract(r"finalTick\s+(\d+)", text)
    runtime_ns = str(int(final_tick) // 1000) if final_tick != "N/A" else "N/A"

    total = 0
    for v in (ap, aap):
        if v != "N/A":
            total += int(v)

    print()
    print("=" * 64)
    print("Results")
    print("  AP  (T, triple-row act) :", ap)
    print("  AAP (O, overlapped act) :", aap)
    print("  Total row-ops           :", total)
    print("  Retries (back-press)    :", retries)
    print("  DRAM MAJ3 (AP) executed :", maj3)
    print("  DRAM RowClones (AAP)    :", clones)
    print("  Runtime (ns)            :", runtime_ns, "(finalTick/1000)")
    print("=" * 64)
except OSError:
    print("(stats file not found at %s)" % stats_path)
