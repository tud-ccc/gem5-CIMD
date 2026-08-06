#ifndef __MEM_ROWOP_TRACE_PLAYER_HH__
#define __MEM_ROWOP_TRACE_PLAYER_HH__

#include <cstdint>
#include <string>
#include <vector>

#include "base/statistics.hh"
#include "mem/request.hh"
#include "mem/trace_player_base.hh"
#include "params/RowOpTracePlayer.hh"

namespace gem5
{

/**
 * RowOpTracePlayer
 *
 * Reads a CIMTRACE binary (from the Cinnamon compiler), expands each
 * MULI / ADDI / ROW_COPY record into the corresponding ROWAP / ROWAAP /
 * ROWCOPY packet sequence, and replays them against a MemCtrl.
 *
 * The pipelined send engine (issue_depth / issue_interval) lives in
 * TracePlayerBase; this class only builds the row-op sequence and maps each
 * entry to a PacketPtr.
 *
 * Address layout for RoRaBaCoCh + DDR4_2400_x64 (banks/rank=16, ranks=2):
 *   ROW_SIZE     = 8192 B (macro from request.hh)
 *   ROWS_PER_VEC = 32     (banksPerRank * ranksPerChannel)
 *   ALIGNMENT    = 262144 B
 *
 *   channel    = global_bank / banks_per_channel
 *   local_bank = global_bank % banks_per_channel
 *   slot_addr(slot, global_bank) =
 *       base_addr + channel * channel_size
 *                 + (slot * banks_per_channel + local_bank) * row_stride
 *
 *   row_stride = device_rowbuffer_size from the DRAM timing config.
 *   For DDR4 row_stride == ROW_SIZE (8192); for HBM2/HBM3 row_stride == 1024.
 *   Using the DRAM row buffer size as the stride ensures MemCtrl's bank/row
 *   decode assigns each (slot, local_bank) pair to the correct DRAM bank and
 *   keeps all slots within a single 512-row subarray.
 *
 *   DDR4 (banks_per_channel=32, 1 channel) : reduces to the original formula.
 *   HBM2 (banks_per_channel=8,  4 channels): global banks 0-7 → ch0, 8-15 → ch1, …
 *   HBM3 (banks_per_channel=16, 2 channels): global banks 0-15 → ch0, 16-31 → ch1.
 *
 *   Slots 0-17 : ambit control rows (T0..C_1, matching init_ambit())
 *   Slots 18+  : data pools (lhs, rhs, out, partial, tmp, carry)
 */
class RowOpTracePlayer : public TracePlayerBase
{
  private:

    // ------------------------------------------------------------------ //
    // Control-row slot indices (must match init_ambit() in mimdram.h)
    // ------------------------------------------------------------------ //
    enum : int {
        SLOT_T0 = 0, SLOT_T1, SLOT_T2, SLOT_T3,
        SLOT_DCC0, SLOT_DCC0N, SLOT_DCC1, SLOT_DCC1N,
        SLOT_DCC0N_T0, SLOT_DCC1N_T1,
        SLOT_T2_T3, SLOT_T0_T3, SLOT_T0_T1_T2, SLOT_T1_T2_T3,
        SLOT_DCC0_T1_T2, SLOT_DCC1_T0_T3,
        SLOT_C_0, SLOT_C_1,
        SLOT_DATA_BASE = 18
    };

    // ROW_SIZE = 8192 is already defined as a macro in request.hh.
    // TOTAL_BANKS = 32 matches mimdram.h BANK_COUNT(16) * RANK_COUNT(2).
    // Banks are partitioned across channels:
    //   channel   = global_bank / banksPerChannel
    //   local_bank = global_bank % banksPerChannel
    static const int TOTAL_BANKS = 32;

    // ------------------------------------------------------------------ //
    // One pending row-op (slot indices; address computed at send time)
    // ------------------------------------------------------------------ //
    struct PendingOp {
        Request::RowOp op;
        int dest_slot;
        int dest_bank;
        int src1_slot;
        int src1_bank;
        int src2_slot; // unused for current Ambit ops
        int src2_bank;
        int macro;     // index into macroOps of the record that emitted this
    };

    // ------------------------------------------------------------------ //
    // One trace record (MULI / ADDI / ROW_COPY) and the row-op span it
    // expands to.  The MacroOp debug flag reports each record's issue
    // window, giving a Gantt granularity above the individual AAP/AP.
    // ------------------------------------------------------------------ //
    struct MacroOp {
        uint8_t  kind;      // OpKind: 0=MULI, 1=ADDI, 2=ROW_COPY
        uint64_t rec;       // record index in the trace file
        int      lhs_bw;
        int      rhs_bw;
        int      nbanks;
        int      src;       // ROW_COPY source bank (else unused)
        int      dst;       // ROW_COPY dest bank   (else unused)
        size_t   npackets;  // row-ops this record expands to
        size_t   sent;      // row-ops issued so far
        std::vector<int> banks;  // global banks this record touches
    };


    // ------------------------------------------------------------------ //
    // Parameters / IDs
    // ------------------------------------------------------------------ //
    const std::string traceFile;
    const Addr        baseAddr;
    const int         banksPerChannel; // banks per MemCtrl instance
    const Addr        channelSize;     // byte stride between channel base addrs
    const Addr        rowStride;       // DRAM row buffer size (address stride per slot)

    // ------------------------------------------------------------------ //
    // Generated row-op sequence.
    //
    // Eager mode (small traces): pendingOps holds every expanded row-op, and
    // makePacket(idx) indexes it directly -- unchanged, bit-identical, and the
    // only mode that supports --per-channel and the MacroOp flag.
    //
    // Lazy mode (huge traces, > LAZY_THRESHOLD packets): pendingOps is reused
    // as a ONE-RECORD buffer.  Only the records are kept (records_, ~50 B each);
    // makePacket(idx) is a forward cursor that expands the next record when idx
    // crosses a record boundary, so peak memory is one record, not the whole
    // (potentially billions-of-packets) sequence.  The single-port engine calls
    // makePacket strictly in increasing idx, which is exactly what the cursor
    // needs.  --per-channel and MacroOp are disabled in lazy mode.
    // ------------------------------------------------------------------ //
    std::vector<PendingOp> pendingOps;

    // Data-pool slot offsets (set from the trace's max widths)
    int lhsBase, rhsBase, outBase, partialBase, tmpBase, carryBase;

    // ------------------------------------------------------------------ //
    // Lazy-mode state (all zero / empty in eager mode)
    // ------------------------------------------------------------------ //
    // Above this many total packets, keep records and expand one at a time.
    static const size_t LAZY_THRESHOLD = 50000000;   // 50 M

    // A stored trace record (kept only in lazy mode; NormRecord equivalent).
    struct Rec {
        uint64_t banks[2];
        int      lhs_bw;
        int      rhs_bw;
        uint8_t  kind;   // OpKind: 0=MULI, 1=ADDI, 2=ROW_COPY
        uint8_t  src;
        uint8_t  dst;
    };
    std::vector<Rec> records_;     // lazy mode only
    bool     lazy_        = false;
    size_t   totalPackets_ = 0;    // exact, from the closed-form count
    int      totalBanks_  = 32;    // 32 or 128, from the trace format
    uint64_t curRec_      = 0;     // next record to expand into the buffer
    size_t   bufBase_     = 0;     // global packet index of pendingOps[0]

    // Closed-form packet count of one record (no materialisation).  Validated
    // to match the eager expansion exactly on all gemv/gemm traces.
    size_t   recordPackets(const Rec& r) const;
    // Expand a single record into pendingOps (the buffer); sets no macro-op.
    void     expandOneRecord(uint64_t rec_idx);

    // Macro-op table + the record currently being expanded (emit* tags with it)
    std::vector<MacroOp> macroOps;
    int curMacro;

    // ------------------------------------------------------------------ //
    // Trace loading + expansion
    // ------------------------------------------------------------------ //
    void loadTrace();
    void expandAdd(int lhs_bw, int rhs_bw, const std::vector<int>& banks);
    void expandMul(int lhs_bw, int rhs_bw, const std::vector<int>& banks);
    void expandRowCopy(int bw, int src_bank, int dst_bank);

    // Primitive emitters
    void emitAAP (int dst_slot, int src_slot, int bank);
    void emitAP  (int dst_slot,               int bank);
    void emitCopy(int dst_slot, int dst_bank, int src_slot, int src_bank);

    // Cross-channel row-copy halves (one full-row data-bus stream per side)
    void emitRdStream(int slot, int bank);
    void emitWrStream(int slot, int bank);

    // Multi-bank composite emitters
    void emitRowAnd(int lhs_slot, int rhs_slot, int out_slot,
                    const std::vector<int>& banks);
    void emitRowAdd(int lhs_slot, int rhs_slot, int out_slot,
                    int cin_slot, int cout_slot,
                    const std::vector<int>& banks);

    // Address + packet helpers
    Addr      slotAddr(int slot, int bank) const;
    PacketPtr makeRowOpPacket(Request::RowOp op,
                               Addr dest, Addr src1, Addr src2);

    // ------------------------------------------------------------------ //
    // TracePlayerBase hooks
    // ------------------------------------------------------------------ //
    void      buildTrace() override { loadTrace(); }
    size_t    numPackets() const override
        { return lazy_ ? totalPackets_ : pendingOps.size(); }
    PacketPtr makePacket(size_t idx) override;
    void      onSent(size_t idx) override;
    // A row-op's packet is routed to its dest bank's channel.
    int       packetChannel(size_t idx) const override
        { return pendingOps[idx].dest_bank / banksPerChannel; }

  public:

    PARAMS(RowOpTracePlayer);
    RowOpTracePlayer(const Params &p);


    struct RowOpTracePlayerStats : public statistics::Group
    {
        RowOpTracePlayerStats(statistics::Group *parent);
        statistics::Scalar numPacketsSent;
    } playerStats;
};

} // namespace gem5

#endif // __MEM_ROWOP_TRACE_PLAYER_HH__
