"""
optipim_player.py  --  replay an OptiPIM Group.txt layout against a MemCtrl

Faithful port of OptiPIM's SimDRAM code generator: reads a Group.txt layout
(kernel + tiled loop nest), expands it the same way OptiPIM's codegen_simdram
does, but issues each MAJ compute op as a ROWAP row-op packet (instead of a
modeled bank-read).  Input loads stay ordinary writes, partial-sum reads stay
ordinary reads.

The DRAM defaults to HBM3 to match OptiPIM's HBM3_PIM model
(banks_per_channel=16, pe_bits=16, dq=128, columns=64).

Usage:
  gem5.opt configs/dram/optipim_player.py --group=/optipim/Result/Group.txt
  gem5.opt configs/dram/optipim_player.py --group=... --bank-mode=broadcast
"""

import argparse
import math
import re
import sys

import m5
from m5.objects import *
from m5.util import addToPath

addToPath("../")

from common import ObjectList

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
ROWS_PER_SUBARRAY = 512  # rows_per_subarray from mimdram.h

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

parser = argparse.ArgumentParser(description=__doc__)

parser.add_argument(
    "--group",
    default="",
    help="Path to the OptiPIM Group.txt layout file (required)",
)
parser.add_argument(
    "--mem-type",
    default="HBM3_4Gb_1x64",
    choices=ObjectList.mem_list.get_names(),
    help="DRAM type (default: HBM3_4Gb_1x64, matches HBM3_PIM)",
)
parser.add_argument(
    "--channels",
    type=int,
    default=8,
    help="Number of memory channels to instantiate (default 8)",
)
parser.add_argument(
    "--bank-mode",
    default="broadcast",
    choices=["broadcast", "single", "serial"],
    help="How the spatial banks are driven: 'broadcast' (default) issues "
    "every row-op to all spatial banks op-major, so the controller paces "
    "the N ACTs at tRRD under tXAW -- the CA-bus cost of an all-bank "
    "row-op is charged; 'single' emits one representative bank and "
    "assumes the rest run free (OptiPIM's own default); 'serial' emits "
    "all banks bank-major, giving zero bank overlap",
)
parser.add_argument(
    "--full-readout",
    action="store_true",
    help="With --bank-mode=single: collapse ROWAP compute only, but emit "
    "partial-sum reads for every spatial bank in the channel (bus "
    "transfers cannot be collapsed)",
)
parser.add_argument(
    "--no-loads",
    action="store_true",
    help="Skip input-load writes; keep MAJ compute and the partial-sum "
    "read-out (reduction cost -- Cinnamon's counterpart is its ROW_COPY "
    "reduction tree). Cinnamon traces carry no input loads",
)
parser.add_argument(
    "--group-by-timestep",
    action="store_true",
    help="Issue each temporal step's packets concurrently "
    "(lockstep-interleaved across channels) with a dependency barrier "
    "between steps -- mirror of the Cinnamon player's start-group engine",
)
parser.add_argument(
    "--sim-timesteps",
    type=int,
    default=-1,
    help="Subsample temporal steps (-1 = no subsampling)",
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
    help="Min ticks between issues (0=no throttle, ~tCK emulates OptiPIM "
    "frontend pacing)",
)
parser.add_argument(
    "--write-buffer-size",
    type=int,
    default=0,
    help="Controller write queue entries (0 = the memory type's default). "
    "Row-ops are write packets, so this is the admission window: when it "
    "is full the port rejects every packet regardless of bank. Must match "
    "the Cinnamon side for a fair comparison.",
)
parser.add_argument(
    "--max-tick",
    type=int,
    default=0,
    help="Stop the simulation after this many ticks (0 = run to completion)",
)

# OptiPIM SimDRAM organization knobs (HBM3_PIM defaults)
parser.add_argument(
    "--rows-per-bank",
    type=int,
    default=ROWS_PER_SUBARRAY,
    help="Rows per bank used to size each channel's address space "
    "(default %d = one subarray). Without --bank-mode=single the "
    "codegen's row offsets grow with spatial elements (~4k rows for GEMV "
    "2048x2048, vs 32k rows/bank in OptiPIM's own HBM3_PIM config); rows "
    "beyond this alias into the next channel's range and eventually fall "
    "off the address map. Use e.g. 8192 for all-bank runs."
    % ROWS_PER_SUBARRAY,
)
parser.add_argument(
    "--maj-model",
    default="cinnamon",
    choices=["cinnamon", "optipim"],
    help="Compute-burst model per output value: 'cinnamon' (Ambit multiply "
    "expansion: AAP=12n^2+4n-7, AP=n^2-1, true AAP/AP mix and order -- "
    "default) or 'optipim' (12n^2-12n+4 ops, all ROWAP -- legacy)",
)
parser.add_argument(
    "--pe-bits",
    type=int,
    default=16,
    help="PE bit precision / MAJ factor (default 16)",
)
parser.add_argument(
    "--dq", type=int, default=128, help="DQ width / values per dq (default 128)"
)
parser.add_argument(
    "--cols", type=int, default=64, help="Columns per row buffer (default 64)"
)

args = parser.parse_args()

if not args.group:
    print("Error: --group is required")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Memory geometry
# ---------------------------------------------------------------------------

intf_cls = ObjectList.mem_list.get(args.mem_type)
tmp = intf_cls()

banks_per_channel = int(tmp.banks_per_rank.value) * int(
    tmp.ranks_per_channel.value
)
num_channels = args.channels

# Effective row buffer size per rank = device_rowbuffer_size * devices_per_rank
# (DDR4: 8 kB, HBM2/HBM3: 1 kB).  Used as the slot row stride.
row_stride = int(tmp.device_rowbuffer_size.getValue()) * int(
    tmp.devices_per_rank.value
)

# DRAM burst size (bytes) = devices_per_rank * burst_length * device_bus_width/8
burst_size = (
    int(tmp.devices_per_rank.value)
    * int(tmp.burst_length.value)
    * int(tmp.device_bus_width.value)
) // 8

# Per-channel address space, rounded up to a power of 2.
channel_size_min = args.rows_per_bank * banks_per_channel * row_stride
channel_size = 1 << int(math.ceil(math.log(channel_size_min, 2)))

base_addr = 0

# ---------------------------------------------------------------------------
# System + clock
# ---------------------------------------------------------------------------

system = System(membus=IOXBar(width=32))
system.clk_domain = SrcClockDomain(
    clock="2.0GHz", voltage_domain=VoltageDomain(voltage="1V")
)

system.mem_ranges = [AddrRange(base_addr, size=num_channels * channel_size)]

# ---------------------------------------------------------------------------
# One controller per channel, non-overlapping address ranges.
# ---------------------------------------------------------------------------

ctrls = []
for c in range(num_channels):
    iface = intf_cls()
    iface.range = AddrRange(base_addr + c * channel_size, size=channel_size)
    ctrl = MemCtrl(dram=iface)
    if args.write_buffer_size > 0:
        ctrl.write_buffer_size = args.write_buffer_size
    ctrls.append(ctrl)

system.mem_ctrls = ctrls
for ctrl in system.mem_ctrls:
    ctrl.port = system.membus.mem_side_ports

# ---------------------------------------------------------------------------
# OptiPimTracePlayer
# ---------------------------------------------------------------------------

system.player = OptiPimTracePlayer(
    group_file=args.group,
    base_addr=base_addr,
    banks_per_channel=banks_per_channel,
    channel_size=channel_size,
    row_stride=row_stride,
    burst_size=burst_size,
    dq=args.dq,
    pe_bits=args.pe_bits,
    n_cols=args.cols,
    bank_mode=args.bank_mode,
    maj_model=args.maj_model,
    full_readout=args.full_readout,
    no_loads=args.no_loads,
    group_by_timestep=args.group_by_timestep,
    sim_timesteps=args.sim_timesteps,
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

print()
print("=" * 64)
print("OptiPIM SimDRAM trace player")
print("  Group file     :", args.group)
print("  Memory type    :", args.mem_type)
print("  Banks/channel  :", banks_per_channel)
print("  Channels       :", num_channels)
print("  Row stride     : %d B" % row_stride)
print("  Burst size     : %d B" % burst_size)
print("  Channel size   : %d MB" % (channel_size >> 20))
print(
    "  pe_bits / dq / cols : %d / %d / %d" % (args.pe_bits, args.dq, args.cols)
)
print("  bank_mode      :", args.bank_mode)
print("  full_readout   :", args.full_readout)
print("  no_loads       :", args.no_loads)
print("  group_by_ts    :", args.group_by_timestep)
print("  issue_depth    : %d  (0=unbounded)" % args.issue_depth)
print("  issue_interval : %d ticks" % args.issue_interval)
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

    rowap = extract(r"system\.player\.numRowAp\s+(\S+)", text)
    rowaap = extract(r"system\.player\.numRowAap\s+(\S+)", text)
    writes = extract(r"system\.player\.numWrites\s+(\S+)", text)
    reads = extract(r"system\.player\.numReads\s+(\S+)", text)
    retries = extract(r"system\.player\.numRetries\s+(\S+)", text)

    act_total = 0
    act_found = False
    for m in re.finditer(
        r"system\.mem_ctrls[^\s]*\.rank\d+\.pwrStateTime::ACT\s+(\d+)", text
    ):
        act_total += int(m.group(1)) // 1000
        act_found = True
    act_str = str(act_total) if act_found else "N/A"

    # Row-op runtime (makespan): the player starts issuing at tick 0 and the
    # sim exits when the last packet responds, so final_tick is the makespan.
    # sim_freq = 1e12 ticks/s, so ns = final_tick / 1000.  This is the metric
    # to compare against Cinnamon's rowOpMakespan ("Row-op runtime (ns)").
    final_tick = extract(r"finalTick\s+(\d+)", text)
    runtime_ns = str(int(final_tick) // 1000) if final_tick != "N/A" else "N/A"

    total = 0
    for v in (rowap, rowaap, writes, reads):
        if v != "N/A":
            total += int(v)

    print()
    print("=" * 64)
    print("Results")
    print("  ROWAP (MAJ compute)   :", rowap)
    print("  ROWAAP (MAJ compute)  :", rowaap)
    print("  Writes (input loads)  :", writes)
    print("  Reads (partial sums)  :", reads)
    print("  Total packets         :", total)
    print("  Retries (back-press)  :", retries)
    print("  Total DRAM ACT (ns)   :", act_str)
    print("  Row-op runtime (ns)   :", runtime_ns, "(final_tick/1000, makespan)")
    print("=" * 64)
except OSError:
    print("(stats file not found at %s)" % stats_path)
