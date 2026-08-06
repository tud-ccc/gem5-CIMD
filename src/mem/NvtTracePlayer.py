from m5.objects.TracePlayerBase import TracePlayerBase
from m5.params import *
from m5.proxy import *


# NvtTracePlayer replays an .nvt row-activation trace: one line per DRAM
# command, "T" for a triple-row activate (AP) and "O" for an overlapped
# activate (AAP).  The pipelined send engine (issue_depth / issue_interval)
# is inherited from TracePlayerBase.
class NvtTracePlayer(TracePlayerBase):
    type = "NvtTracePlayer"
    cxx_header = "mem/nvt_trace_player.hh"
    cxx_class = "gem5::NvtTracePlayer"

    trace_file = Param.String("Path to the .nvt trace file")

    base_addr = Param.Addr(0, "Base address of the replayed row space")

    # Address granularity in the trace.  Every address in the file is a
    # multiple of this; dividing by it yields a dense row index.
    trace_row_size = Param.Addr(
        1024, "Byte granularity of addresses in the trace"
    )

    # gem5 address-mapping geometry (same meaning as RowOpTracePlayer):
    # consecutive row_stride steps walk banks, not rows, under the
    # RoRaBa* mappings, so a row index is placed with
    #   addr = base + (row * banks_per_channel + bank) * row_stride
    banks_per_channel = Param.Int(32, "Banks per memory controller")
    row_stride = Param.Addr(8192, "DRAM row buffer size in bytes")

    # Bank the whole trace is replayed into.  An AAP copies through the
    # shared sense amplifiers, so its source and destination must live in
    # the same bank (and subarray); the trace carries no bank information,
    # so every row of it lands in one bank.
    bank = Param.Int(0, "Bank index the trace is replayed into")

    # The traces touch few rows out of a wide address span (1024.nvt uses
    # 118 rows spread over indices 0..3845). Packing each distinct address
    # into the next free row keeps the working set inside one subarray --
    # which is what an AAP through shared sense amplifiers requires -- and
    # keeps the address range small. Turn off to use the raw
    # address/trace_row_size index, which needs a much larger range.
    compact_rows = Param.Bool(
        True, "Pack distinct trace addresses into dense row indices"
    )
