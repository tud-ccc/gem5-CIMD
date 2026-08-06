#ifndef __MEM_NVT_TRACE_PLAYER_HH__
#define __MEM_NVT_TRACE_PLAYER_HH__

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "mem/request.hh"
#include "mem/trace_player_base.hh"
#include "params/NvtTracePlayer.hh"

namespace gem5
{

/**
 * NvtTracePlayer
 *
 * Replays an .nvt row-activation trace against a memory controller.  Each
 * line of the file is one DRAM command:
 *
 *   <seq> T <row> <data> 0            triple-row activate  -> ROWAP
 *   <seq> O <src> <data> 0 <dst>      overlapped activate  -> ROWAAP
 *
 * The <data> column is the 64-byte row content the generator expected. The
 * replay is timing-only (row-ops carry no payload bytes through the DRAM
 * model here), so it is parsed for validation of the line shape and then
 * ignored.  The fifth column is always 0 in the traces seen so far.
 *
 * Addresses are dense multiples of trace_row_size; the row index
 * (addr / trace_row_size) is placed into a single bank with the same slot
 * scheme RowOpTracePlayer uses:
 *
 *   addr = base_addr + (row * banks_per_channel + bank) * row_stride
 *
 * One bank, because an AAP copies through shared sense amplifiers: source
 * and destination have to be in the same bank and subarray, and the trace
 * carries no bank information of its own.
 *
 * The pipelined send engine (issue_depth / issue_interval) lives in
 * TracePlayerBase; this class only builds the row-op sequence and maps each
 * entry to a PacketPtr.
 */
class NvtTracePlayer : public TracePlayerBase
{
  private:

    // One parsed command.  Row indices, not addresses: the address is
    // computed at send time by slotAddr().
    struct PendingOp
    {
        Request::RowOp op;    // ROWAP or ROWAAP
        uint32_t dest_row;    // T: the activated row; O: the destination
        uint32_t src_row;     // O only; unused for T
    };

    // ------------------------------------------------------------------ //
    // Parameters
    // ------------------------------------------------------------------ //
    const std::string traceFile;
    const Addr        baseAddr;
    const Addr        traceRowSize;
    const int         banksPerChannel;
    const Addr        rowStride;
    const int         bank;
    const bool        compactRows;

    // Parsed command sequence
    std::vector<PendingOp> pendingOps;

    // Trace address -> dense row index, in order of first appearance.
    // The traces touch few rows out of a wide address span (1024.nvt: 118
    // rows spread over indices 0..3845), and an AAP is only meaningful
    // within one subarray, so packing them keeps every operand close
    // together instead of scattering it across subarrays.
    std::unordered_map<Addr, uint32_t> rowIndex;

    // ------------------------------------------------------------------ //
    // Trace loading + helpers
    // ------------------------------------------------------------------ //
    void      loadTrace();
    uint32_t  rowFor(Addr trace_addr);
    Addr      slotAddr(uint32_t row) const;
    PacketPtr makeRowOpPacket(Request::RowOp op, Addr dest, Addr src1);

    // ------------------------------------------------------------------ //
    // TracePlayerBase hooks
    // ------------------------------------------------------------------ //
    void      buildTrace() override { loadTrace(); }
    size_t    numPackets() const override { return pendingOps.size(); }
    PacketPtr makePacket(size_t idx) override;
    void      onSent(size_t idx) override;

  public:
    PARAMS(NvtTracePlayer);
    NvtTracePlayer(const Params &p);

    // ------------------------------------------------------------------ //
    // Statistics
    // ------------------------------------------------------------------ //
    struct NvtTracePlayerStats : public statistics::Group
    {
        NvtTracePlayerStats(statistics::Group *parent);
        statistics::Scalar numAp;
        statistics::Scalar numAap;
    } playerStats;
};

} // namespace gem5

#endif // __MEM_NVT_TRACE_PLAYER_HH__
