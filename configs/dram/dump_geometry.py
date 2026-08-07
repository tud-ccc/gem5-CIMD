"""
dump_geometry.py  --  print a DRAM class's geometry as a PuD-style config dict

Converts a gem5 DRAMInterface timing class into the flat geometry dict that
subarray-aware simulators (scheme "C:R:SA:BK:RK:CH") expect:

    num_channels, num_ranks, num_banks, num_subarrays, num_rows, num_cols

Two conventions have to be pinned down, because gem5 does not name them the
same way:

  num_rows  -- rows per BANK (not per subarray). num_subarrays is then a
               partition of those rows: rows_per_subarray = num_rows /
               num_subarrays.
  num_cols  -- bits per row (row buffer size in bits) by default, which is
               what Ambit-style tools mean by a column. Pass --cols-unit=burst
               to get gem5's own notion instead (bursts per row buffer).

Usage:
  gem5.opt configs/dram/dump_geometry.py --mem-type=HBM2_4Gb_1x64
  gem5.opt configs/dram/dump_geometry.py --mem-type=HBM2_4Gb_1x64 \
      --rows-per-subarray=1024 --channels=16
"""

import argparse
import sys

import m5
from m5.objects import *
from m5.util import addToPath

addToPath("../")

from common import ObjectList

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    "--mem-type",
    default="HBM2_4Gb_1x64",
    choices=ObjectList.mem_list.get_names(),
    help="DRAM timing class to convert (default: HBM2_4Gb_1x64)",
)
parser.add_argument(
    "--rows-per-subarray",
    type=int,
    default=1024,
    help="Rows per subarray used to derive num_subarrays (default 1024). "
    "MIMDRAM uses 512; gem5-CIMD's rows_per_subarray param defaults to 1024.",
)
parser.add_argument(
    "--channels",
    type=int,
    default=1,
    help="Channels to report (default 1 = one pseudo-channel). An HBM2/HBM3 "
    "stack in pseudo-channel mode has 16.",
)
parser.add_argument(
    "--cols-unit",
    default="bit",
    choices=["bit", "byte", "burst"],
    help="What one column counts as: 'bit' (row bits, Ambit convention, "
    "default), 'byte' (row bytes), or 'burst' (gem5's columnsPerRowBuffer)",
)
args = parser.parse_args()

cls = ObjectList.mem_list.get(args.mem_type)
d = cls()

# --- raw gem5 parameters -------------------------------------------------
device_size = int(d.device_size.getValue())  # bytes per device
row_bytes = int(d.device_rowbuffer_size.getValue())
devices_per_rank = int(d.devices_per_rank.value)
ranks_per_channel = int(d.ranks_per_channel.value)
banks_per_rank = int(d.banks_per_rank.value)
bank_groups = int(d.bank_groups_per_rank.value)
bus_width = int(d.device_bus_width.value)
burst_length = int(d.burst_length.value)

# --- derived -------------------------------------------------------------
# gem5: rowsPerBank = capacity / (rowBufferSize * banksPerRank * ranks)
capacity = device_size * devices_per_rank
rows_per_bank = capacity // (
    row_bytes * devices_per_rank * banks_per_rank * ranks_per_channel
)

# A rank presents devices_per_rank devices in lockstep, so the row seen by
# the controller is wider than one device's row buffer.
rank_row_bytes = row_bytes * devices_per_rank
burst_bytes = devices_per_rank * burst_length * bus_width // 8

if args.cols_unit == "bit":
    num_cols = rank_row_bytes * 8
elif args.cols_unit == "byte":
    num_cols = rank_row_bytes
else:
    num_cols = rank_row_bytes // burst_bytes

num_subarrays = rows_per_bank // args.rows_per_subarray

print()
print("# %s  (%s)" % (args.mem_type, cls.__name__))
print("#   device_size            = %d MiB" % (device_size >> 20))
print("#   device_rowbuffer_size  = %d B" % row_bytes)
print("#   devices_per_rank       = %d" % devices_per_rank)
print("#   bank_groups_per_rank   = %d" % bank_groups)
print("#   burst                  = %d B (BL%d x %d-bit bus)"
      % (burst_bytes, burst_length, bus_width))
print("#   rows/bank              = capacity / (row_bytes * banks * ranks)")
print("#                          = %d" % rows_per_bank)
print("#   subarrays              = rows/bank / %d = %d"
      % (args.rows_per_subarray, num_subarrays))
print("#   columns                = %d (%s per row)" % (num_cols, args.cols_unit))
print()
print("DRAM_config = {")
print('    "scheme": "C:R:SA:BK:RK:CH",')
print('    "num_channels": %d,' % args.channels)
print('    "num_ranks": %d,' % ranks_per_channel)
print('    "num_banks": %d,' % banks_per_rank)
print('    "num_subarrays": %d,' % num_subarrays)
print('    "num_rows": %d,' % rows_per_bank)
print('    "num_cols": %d' % num_cols)
print("}")
print()

# capacity cross-check: the dict must describe the same silicon
bits = (
    args.channels
    * ranks_per_channel
    * banks_per_rank
    * rows_per_bank
    * rank_row_bytes
    * 8
)
print(
    "# cross-check: %d ch x %d rk x %d bk x %d rows x %d B/row = %.2f MiB"
    % (
        args.channels,
        ranks_per_channel,
        banks_per_rank,
        rows_per_bank,
        rank_row_bytes,
        bits / 8 / (1 << 20),
    )
)
print()
sys.exit(0)
