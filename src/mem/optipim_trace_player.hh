#ifndef __MEM_OPTIPIM_TRACE_PLAYER_HH__
#define __MEM_OPTIPIM_TRACE_PLAYER_HH__

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/statistics.hh"
#include "mem/optipim_layout.hh"
#include "mem/request.hh"
#include "mem/trace_player_base.hh"
#include "params/OptiPimTracePlayer.hh"

namespace gem5
{

/**
 * OptiPimTracePlayer
 *
 * Reads an OptiPIM "Group.txt" layout (kernel + tiled loop nest), runs a
 * faithful port of OptiPIM's SimDRAM code generator, and replays the result
 * against a bare MemCtrl.  Each generated access becomes one gem5 packet:
 *
 *   input load  (write)     -> ordinary WriteReq
 *   partial sum (read)      -> ordinary ReadReq
 *   compute (bank-read)     -> ROWAAP / ROWAP row-op packets
 *
 * The compute burst per output value is selected by maj_model:
 *
 *   "cinnamon" (default) -- the Ambit multiply expansion RowOpTracePlayer
 *       emits for a MULI of lhs_bw = rhs_bw = n, op for op and in order:
 *           rowAnd = 4 AAP,  rowAdd = 8 AAP + 1 AP
 *           AAP = 12n^2 + 4n - 7,  AP = n^2 - 1,  total = 13n^2 + 4n - 8
 *       (n=8: 793 AAP + 63 AP = 856 ops, ~7.4% AP).  Both players are then
 *       charged the same primitive mix -- which matters because gem5 prices
 *       ROWAP at 24 ns but ROWAAP at 29 ns.
 *
 *   "optipim" -- OptiPIM's own MAJ count, 12*pe_bits^2 - 12*pe_bits + 4 ops
 *       (676 for n=8), every one issued as a ROWAP.  Faithful to their
 *       codegen's op count but wrong in mix: a real Ambit multiply is ~93%
 *       AAP, so an all-ROWAP burst undercharges every op.
 *
 * The pipelined send engine (issue_depth / issue_interval) lives in
 * TracePlayerBase; this class only builds the packet sequence and maps each
 * entry to a PacketPtr.
 *
 * Addresses reuse RowOpTracePlayer's per-channel slot scheme:
 *   channel    = global_bank / banksPerChannel
 *   local_bank = global_bank % banksPerChannel
 *   slotAddr(row, global_bank) =
 *       base + channel*channelSize + (row*banksPerChannel + local_bank)*rowStride
 * with a column byte offset added within the row for plain read/write.
 */
class OptiPimTracePlayer : public TracePlayerBase
{
  private:

    // One pending packet (address resolved; kind selects the gem5 cmd)
    enum Kind : uint8_t { K_ROWAP, K_ROWAAP, K_WRITE, K_READ };

    struct PendingPkt {
        Kind     kind;
        Addr     addr;   // gem5 physical address (dest row)
        Addr     src1;   // K_ROWAAP source row; unused by the other kinds
        uint32_t macro;  // index into macroOps; survives interleaveByChannel
    };

    // ------------------------------------------------------------------ //
    // Macro-op: one phase of one (bank, row) in the codegen.  LOAD is the
    // input broadcast, READOUT the partial-sum burst, COMPUTE the MAJ chain
    // for one output value.  The MacroOp debug flag reports each one's issue
    // window, giving a Gantt granularity above the individual ROWAP.
    // ------------------------------------------------------------------ //
    enum MacroKind : uint8_t { M_LOAD, M_READOUT, M_COMPUTE };

    struct MacroOp {
        MacroKind kind;
        int       bank;      // source bank id
        int       row;       // row offset within the bank
        size_t    npackets;
        size_t    sent;
    };


    // Open a macro-op and make it current; close it, dropping it if empty.
    uint32_t beginMacro(MacroKind k, int bank, int row);
    void     endMacro(uint32_t id);

    // ------------------------------------------------------------------ //
    // Parameters
    // ------------------------------------------------------------------ //
    const std::string groupFile;
    const Addr        baseAddr;
    const int         banksPerChannel;
    const Addr        channelSize;
    const Addr        rowStride;
    const int         burstSize;
    const int         dq;
    const int         peBits;
    const int         nCols;
    // How the spatial banks of a timestep are driven.  See bank_mode in
    // OptiPimTracePlayer.py; these are the three modes OptiPIM's own simulator
    // can be put in, made explicit:
    //   BM_BROADCAST -- each row-op issued to ALL spatial banks, op-major /
    //                   bank-minor, so MemCtrl paces the N ACTs at tRRD under
    //                   the tXAW cap.  The physically correct model of an
    //                   all-bank row-op on a shared CA bus.  (default)
    //   BM_SINGLE    -- OptiPIM's single_bank_opt=true: emit one representative
    //                   bank and assume the other N-1 run in lockstep for free.
    //                   No CA-bus cost at all -- optimistic.
    //   BM_SERIAL    -- OptiPIM's single_bank_opt=false: emit every bank
    //                   bank-major, which the in-order pump then drains one
    //                   bank at a time -- pessimistic (zero bank overlap).
    enum BankMode : uint8_t { BM_BROADCAST, BM_SINGLE, BM_SERIAL };
    BankMode          bankMode;

    // Spatial banks of the current timestep, and the first of them.  Set by
    // codegenSimdram from the layout; used to replicate a row-op across banks.
    int               spatialBanks;
    const int         simTimesteps;
    const bool        fullReadout;
    // Skip input-load writes (compute + partial-sum read-out only).  The
    // read-out is kept: it is the reduction cost, whose Cinnamon-side
    // counterpart (the ROW_COPY reduction tree) stays in its traces too.
    const bool        noLoads;
    // Issue one temporal step's packets concurrently with a dependency
    // barrier between steps (spatial banks = parallel PUs, sequential_steps
    // = true dependency) -- the mirror of RowOpTracePlayer's start-groups.
    // Off = legacy free-run in strict program (bank-major) order.
    const bool        groupByTimestep;
    // "cinnamon" (Ambit multiply expansion) or "optipim" (12n^2-12n+4 ROWAPs)
    const std::string majModel;

    // Generated packet sequence
    std::vector<PendingPkt> pendingPkts;

    // rows-per-bank (count[subarray]*count[row])
    int rowsPerBank;

    // Banks whose partial-sum read-out is emitted per collapsed bank.
    // 1 unless full_readout && single_bank_opt, then the spatial banks
    // sharing the channel (their read bursts serialise on the channel bus,
    // unlike ROWAP compute which is a legitimate all-bank broadcast).
    int readoutBanks;

    // Macro-op table, the one being generated, and where its packets started.
    std::vector<MacroOp> macroOps;
    uint32_t curMacro;
    size_t   macroStart;

    // ------------------------------------------------------------------ //
    // Trace loading: parse Group.txt -> Layout -> codegen -> pendingPkts
    // ------------------------------------------------------------------ //
    optipim::Layout* parseGroup(const std::string& path);
    void codegenSimdram(optipim::Layout* layout);
    // Lockstep-merge pendingPkts[from..end) round-robin across channels,
    // keeping each channel's internal (per-bank chain) order.  Makes the
    // in-order pump reach every channel of an issue group instead of
    // draining the bank-major stream one channel at a time.
    void interleaveByChannel(size_t from);
    void codegenBank(int global_bank_id,
                     const std::map<int, std::map<int, int>>& row_col_accesses,
                     bool output_tensor, bool weight_tensor,
                     bool first_time_in_col);

    // ------------------------------------------------------------------ //
    // Compute-burst model (see maj_model in OptiPimTracePlayer.py)
    //
    // emitComputeBurst() appends the row-ops for ONE output value on one
    // bank.  "optipim" = 12n^2-12n+4 ROWAPs.  "cinnamon" = the Ambit multiply
    // expansion RowOpTracePlayer emits, op for op and in order, so the AAP/AP
    // mix (and therefore the per-op latency) matches between the two players.
    // ------------------------------------------------------------------ //
    void emitComputeBurst(int row_addr, int first_bank, uint32_t macro);
    // Cinnamon primitives: 4 AAP, and 8 AAP + 1 AP respectively.
    void emitCinRowAnd(int row_addr, int bank, uint32_t macro, int& cur_row);
    void emitCinRowAdd(int row_addr, int bank, uint32_t macro, int& cur_row);
    // One row-op of the compute burst.  Under BM_BROADCAST this emits the SAME
    // op to every spatial bank back-to-back (op-major / bank-minor), which is
    // what lets the banks pipeline; cur_row advances once per op, not per bank,
    // since all banks execute the identical row-op.
    void emitComputeOp(Kind k, int row_addr, int first_bank, uint32_t macro,
                       int& cur_row);

    // Address + packet helpers
    int       addToBankTag(int row_offset, int global_bank_id) const;
    Addr      slotAddr(int row, int global_bank, int col) const;
    PacketPtr makeRowApPacket(Addr dest);
    PacketPtr makeRowAapPacket(Addr dest, Addr src1);
    PacketPtr makeMemPacket(Addr addr, bool isWrite);

    // ------------------------------------------------------------------ //
    // TracePlayerBase hooks
    // ------------------------------------------------------------------ //
    void      buildTrace() override;
    size_t    numPackets() const override { return pendingPkts.size(); }
    PacketPtr makePacket(size_t idx) override;
    void      onSent(size_t idx) override;

  public:

    PARAMS(OptiPimTracePlayer);
    OptiPimTracePlayer(const Params &p);


    struct OptiPimTracePlayerStats : public statistics::Group
    {
        OptiPimTracePlayerStats(statistics::Group *parent);
        statistics::Scalar numRowAp;
        statistics::Scalar numRowAap;
        statistics::Scalar numWrites;
        statistics::Scalar numReads;
    } playerStats;
};

} // namespace gem5

#endif // __MEM_OPTIPIM_TRACE_PLAYER_HH__
