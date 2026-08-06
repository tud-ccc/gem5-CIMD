from m5.objects.TracePlayerBase import TracePlayerBase
from m5.params import *
from m5.proxy import *


# OptiPimTracePlayer
#
# Reads an OptiPIM "Group.txt" layout description (kernel + tiled loop nest)
# and replays the SimDRAM code generator against a bare MemCtrl.  Faithful
# port of OptiPIM's SimDRAM codegen, with one change: each MAJ compute op is
# issued as a ROWAP row-op packet instead of a modeled bank-read.
#
# Input/output loads stay ordinary write/read accesses.  Addresses use the
# same per-channel slotAddr scheme as RowOpTracePlayer.  The pipelined send
# engine (issue_depth / issue_interval) is inherited from TracePlayerBase.
class OptiPimTracePlayer(TracePlayerBase):
    type = "OptiPimTracePlayer"
    cxx_header = "mem/optipim_trace_player.hh"
    cxx_class = "gem5::OptiPimTracePlayer"

    group_file = Param.String("Path to the OptiPIM Group.txt layout file")

    base_addr = Param.Addr(0, "Base address (start of channel 0)")

    # --- gem5 address-mapping geometry (same meaning as RowOpTracePlayer) ---
    banks_per_channel = Param.Int(16, "Banks per DRAMCtrl instance")
    channel_size = Param.Addr(0, "Address range per channel (bytes)")
    row_stride = Param.Addr(1024, "DRAM row buffer size in bytes (row stride)")
    burst_size = Param.Int(32, "DRAM burst size in bytes (read/write access)")

    # --- DRAM organization seen by the SimDRAM codegen (HBM3_PIM defaults) ---
    dq = Param.Int(128, "DQ width / values per dq (m_organization.dq)")
    pe_bits = Param.Int(16, "PE bit precision (n rows per value / MAJ factor)")
    n_cols = Param.Int(64, "Columns per row buffer (count[column])")

    # --- codegen options (mirror simdram_config.yaml) ---
    # How the spatial banks of a timestep are driven.  These are the three
    # modes OptiPIM's own simulator can be put in, made explicit:
    #
    #   "broadcast" (default) -- each row-op is issued to ALL spatial banks,
    #       op-major / bank-minor, so the controller paces the N ACTs at tRRD under
    #       the tXAW cap.  A SIMDRAM row-op is bank-internal, so driving N banks
    #       needs N ACTs on a shared CA bus; this is the only mode that charges
    #       for that.  Banks pipeline, but not for free.
    #
    #   "single" -- OptiPIM's single_bank_opt=true (their DEFAULT, and what
    #       their validation/validation_gemm.py runs).  Emits one representative
    #       bank and assumes the other N-1 execute in lockstep at zero cost.
    #       Optimistic: no CA-bus cost at all.
    #
    #   "serial" -- OptiPIM's single_bank_opt=false.  Emits every bank
    #       bank-major; the in-order pump then drains one bank's whole chain
    #       before the next starts.  Pessimistic: zero bank overlap.
    bank_mode = Param.String("broadcast",
        "How spatial banks are driven: 'broadcast' (row-op issued to all banks, "
        "op-major, paced at tRRD), 'single' (one representative bank, lockstep "
        "assumed free -- OptiPIM's default), or 'serial' (all banks bank-major, "
        "no overlap)")
    sim_timesteps = Param.Int(-1,
        "Subsample temporal steps to this many (-1 = no subsampling)")
    full_readout = Param.Bool(False,
        "With single_bank_opt: still collapse ROWAP compute to one "
        "representative bank (all-bank SIMD), but emit the partial-sum "
        "read-out for every spatial bank in the channel -- bank data "
        "transfers share the channel bus and cannot be collapsed")
    no_loads = Param.Bool(False,
        "Skip input-load writes; keep MAJ compute and the partial-sum "
        "read-out (the reduction cost, whose Cinnamon-side counterpart "
        "is the ROW_COPY reduction tree).  For compute+reduction-only "
        "comparisons against Cinnamon traces, which carry no input loads")
    group_by_timestep = Param.Bool(False,
        "Issue each temporal step's packets concurrently with a dependency "
        "barrier between steps (mirror of RowOpTracePlayer's start-group "
        "engine: spatial banks are independent PUs, sequential steps are a "
        "true dependency).  Off = legacy free-run in strict program order, "
        "which serialises the bank-major stream onto one channel at a time")

    # --- how the per-output-value compute burst is modelled ---
    #
    # "optipim"  : OptiPIM's own MAJ count, 12n^2 - 12n + 4 ops, every one
    #              issued as a ROWAP.  Faithful to their codegen's op count,
    #              but wrong in mix: a real Ambit multiply is ~93% AAP, and
    #              gem5 prices ROWAP at 24 ns vs ROWAAP at 29 ns, so an
    #              all-ROWAP burst undercharges every op.
    #
    # "cinnamon" : the Ambit multiply expansion RowOpTracePlayer actually
    #              emits, op for op and in order --
    #                  rowAnd = 4 AAP,  rowAdd = 8 AAP + 1 AP
    #                  AAP = 12n^2 + 4n - 7,  AP = n^2 - 1
    #                  total = 13n^2 + 4n - 8   (856 for n=8, vs 676)
    #              Costlier and more numerous, but it is the same primitive
    #              mix both players are charged for.
    maj_model = Param.String("cinnamon",
        "Compute-burst model per output value: 'cinnamon' (Ambit multiply "
        "expansion, AAP/AP in true proportion and order) or 'optipim' "
        "(12n^2-12n+4 ops, all ROWAP -- the legacy behaviour)")
