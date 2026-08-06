"""
trace_player.py  --  replay a CIMTRACE binary against a bare MemCtrl

Two CIMTRACE binary formats are supported, auto-detected from the header:

  record_size==32: 32-bank format  (93_simdram_schedule_runner.c)
  record_size==48: 128-bank format (94_simdram_schedule_runner_hbm.c)

Memory type to channel mapping:

  32-bank traces:
    DDR4_2400_8x8   : 32 banks/ch x 1 channel
    HBM3_*_1x64     : 16 banks/ch x 2 channels
    HBM2_*_1x64     :  8 banks/ch x 4 channels

  128-bank traces:
    HBM3_*_1x64     : 16 banks/ch x 8 channels
    HBM2_*_1x64     :  8 banks/ch x 16 channels

Usage:
  gem5.opt configs/dram/trace_player.py --trace=microworkloads/trace.bin
  gem5.opt configs/dram/trace_player.py --trace=... --mem-type=HBM2_4Gb_1x64
  gem5.opt configs/dram/trace_player.py --trace=... --mem-type=HBM3_4Gb_1x64
"""

import argparse
import math
import re
import struct
import sys

import m5
from m5.objects import *
from m5.util import addToPath, fatal

addToPath("../")

from common import ObjectList

# ---------------------------------------------------------------------------
# Constants (must match mimdram.h and request.hh)
# ---------------------------------------------------------------------------
ROWS_PER_SUBARRAY = 512  # rows_per_subarray from mimdram.h
ROW_SIZE_BYTES = 8192  # CIM_ROW_SIZE in request.hh

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

parser = argparse.ArgumentParser(description=__doc__)

parser.add_argument(
    "--trace", default="", help="Path to the CIMTRACE binary file (required)"
)
parser.add_argument(
    "--mem-type",
    default="DDR4_2400_8x8",
    choices=ObjectList.mem_list.get_names(),
    help="DRAM type (default: DDR4_2400_8x8). banks_per_rank * "
    "ranks_per_channel must divide the trace total_banks (32 or 128).",
)
parser.add_argument(
    "--addr-map",
    default="",
    help="Override addr_mapping (e.g. RoRaBaCoCh, RoRaBaChCo). "
    "Empty = use the memory type's default.",
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
    "--max-tick",
    type=int,
    default=0,
    help="Stop the simulation after this many ticks (0 = run to completion)",
)
parser.add_argument(
    "--membus-width",
    type=int,
    default=32,
    help="IOXBar membus width in bytes (default 32); raise to probe "
    "interconnect throughput limits",
)
parser.add_argument(
    "--membus-clock",
    default="2.0GHz",
    help="Membus/system clock (default 2.0GHz)",
)
parser.add_argument(
    "--write-buffer-size",
    type=int,
    default=0,
    help="Controller write queue entries (0 = the memory type's default). "
    "Row-ops are write packets, so this is the admission window: when it "
    "is full the port rejects every packet regardless of bank, and the "
    "player stalls.",
)
parser.add_argument(
    "--per-channel",
    action="store_true",
    default=False,
    help="Use the per-channel issue engine (one player port per channel, "
    "each with its own retry slot) to remove single-port head-of-line "
    "blocking",
)

args = parser.parse_args()

if not args.trace:
    print("Error: --trace is required")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Auto-detect total bank count from the trace header.
#   record_size == 32  ->  32-bank format  (uint32_t banks bitmask)
#   record_size == 48  ->  128-bank format (uint64_t banks[2] bitmask)
# ---------------------------------------------------------------------------

try:
    with open(args.trace, "rb") as _tf:
        _hdr = _tf.read(32)
except OSError as e:
    print("Error: cannot open trace file '%s': %s" % (args.trace, e))
    sys.exit(1)

if len(_hdr) < 32 or _hdr[:8] != b"CIMTRACE":
    print("Error: '%s' is not a valid CIMTRACE file" % args.trace)
    sys.exit(1)

_record_size = struct.unpack_from("<I", _hdr, 12)[0]
if _record_size == 32:
    total_banks = 32
elif _record_size == 48:
    total_banks = 128
else:
    print(
        "Error: unrecognised record_size=%d in '%s'"
        % (_record_size, args.trace)
    )
    sys.exit(1)

# ---------------------------------------------------------------------------
# Memory geometry
# ---------------------------------------------------------------------------

intf_cls = ObjectList.mem_list.get(args.mem_type)
tmp = intf_cls()

banks_per_channel = int(tmp.banks_per_rank.value) * int(
    tmp.ranks_per_channel.value
)

if total_banks % banks_per_channel != 0:
    fatal(
        "'%s' has %d banks/channel (%d banks/rank x %d ranks), which does "
        "not divide total_banks=%d (from trace).  Choose a type whose "
        "banks_per_rank x ranks_per_channel divides %d."
        % (
            args.mem_type,
            banks_per_channel,
            int(tmp.banks_per_rank.value),
            int(tmp.ranks_per_channel.value),
            total_banks,
            total_banks,
        )
    )

num_channels = total_banks // banks_per_channel

# row_stride: the effective DRAM row buffer size per rank
# (= device_rowbuffer_size * devices_per_rank) determines the address stride
# between consecutive slot rows in slotAddr().  Using this (not application
# ROW_SIZE) ensures the controller's bank/row decode assigns the correct DRAM
# bank to each address and keeps all slot rows within a single 512-row
# subarray.
#   DDR4:       1 kB/dev x 8 devs/rank = 8 kB  (= ROW_SIZE, no change)
#   HBM2/HBM3:  1 kB/dev x 1 dev/rank  = 1 kB
row_stride = int(tmp.device_rowbuffer_size.getValue()) * int(
    tmp.devices_per_rank.value
)

# Per-channel address space: ROWS_PER_SUBARRAY row-buffer rows per local bank.
# Rounded up to the next power of 2 so the geometry checks pass.
channel_size_min = ROWS_PER_SUBARRAY * banks_per_channel * row_stride
channel_size = 1 << int(math.ceil(math.log(channel_size_min, 2)))

base_addr = 0

# ---------------------------------------------------------------------------
# System + clock
# ---------------------------------------------------------------------------

system = System(membus=IOXBar(width=args.membus_width))
system.clk_domain = SrcClockDomain(
    clock=args.membus_clock, voltage_domain=VoltageDomain(voltage="1V")
)

system.mem_ranges = [AddrRange(base_addr, size=num_channels * channel_size)]

# ---------------------------------------------------------------------------
# One controller per channel, non-overlapping address ranges.
# Using standalone (non-interleaved) ranges avoids the XOR hash that
# MemConfig injects for multi-channel configs, keeping the bank->channel
# mapping fully deterministic.
# ---------------------------------------------------------------------------

ctrls = []
for c in range(num_channels):
    iface = intf_cls()
    if args.addr_map:
        iface.addr_mapping = args.addr_map
    iface.range = AddrRange(base_addr + c * channel_size, size=channel_size)
    if args.write_buffer_size > 0:
        iface.write_buffer_size = args.write_buffer_size
    ctrl = MemCtrl(dram=iface)
    ctrls.append(ctrl)

system.mem_ctrls = ctrls

for ctrl in system.mem_ctrls:
    ctrl.port = system.membus.mem_side_ports

# ---------------------------------------------------------------------------
# RowOpTracePlayer
# ---------------------------------------------------------------------------

system.player = RowOpTracePlayer(
    trace_file=args.trace,
    base_addr=base_addr,
    banks_per_channel=banks_per_channel,
    channel_size=channel_size,
    row_stride=row_stride,
    per_channel=args.per_channel,
    issue_depth=args.issue_depth,
    issue_interval=args.issue_interval,
)

# Single-port path is always bound (idle in per-channel mode).
system.player.port = system.membus.cpu_side_ports
if args.per_channel:
    # One request port per channel; the membus routes each by address to its
    # controller, and each port carries its own retry slot.
    system.player.chan_port = [
        system.membus.cpu_side_ports for _ in range(num_channels)
    ]
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
print("RowOp trace player")
print("  Trace          :", args.trace)
print("  Memory type    :", args.mem_type)
print(
    "  Banks/channel  : %d  (%d banks/rank x %d ranks)"
    % (
        banks_per_channel,
        int(tmp.banks_per_rank.value),
        int(tmp.ranks_per_channel.value),
    )
)
print("  Trace format   : %d-bank" % total_banks)
print("  Channels       :", num_channels)
print("  Total banks    :", banks_per_channel * num_channels)
print("  Row stride     : %d B  (DRAM row buffer size)" % row_stride)
print("  Channel size   : %d MB" % (channel_size >> 20))
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

    pkts = extract(r"system\.player\.numPacketsSent\s+(\S+)", text)
    retries = extract(r"system\.player\.numRetries\s+(\S+)", text)
    wr_reqs = extract(r"system\.mem_ctrls\.writeReqs\s+(\S+)", text)

    # Sum ACT ticks across all channel controllers
    act_total = 0
    act_found = False
    for m in re.finditer(
        r"system\.mem_ctrls[^\s]*\.rank\d+\.pwrStateTime::ACT\s+(\d+)", text
    ):
        act_total += int(m.group(1)) // 1000
        act_found = True
    act_str = str(act_total) if act_found else "N/A"

    print()
    print("=" * 64)
    print("Results")
    print("  Packets sent          :", pkts)
    print("  Retries (back-press)  :", retries)
    print("  DRAM write reqs (ch0) :", wr_reqs)
    print("  Total DRAM ACT ticks  :", act_str)
    print("=" * 64)
except OSError:
    print("(stats file not found at %s)" % stats_path)
