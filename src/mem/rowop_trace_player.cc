#include "mem/rowop_trace_player.hh"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "base/trace.hh"
#include "debug/MacroOp.hh"
#include "debug/RowOpTracePlayer.hh"
#include "mem/packet.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5
{

// ---------------------------------------------------------------------------
// On-disk trace structures.
//
// Two binary formats exist, distinguished by TraceHeader::record_size:
//
//   record_size == 32  ->  32-bank format  (93_simdram_schedule_runner.c)
//                          banks field: uint32_t (bits 0..31)
//
//   record_size == 48  ->  128-bank format (94_simdram_schedule_runner_hbm.c)
//                          banks field: uint64_t[2] (bits 0..127)
//
// After reading, both are normalised into NormRecord for uniform processing.
// ---------------------------------------------------------------------------
namespace {

struct TraceHeader {
    char     magic[8];
    uint32_t version;
    uint32_t record_size;
    uint64_t num_records;
    int64_t  last_end_time;
};
static_assert(sizeof(TraceHeader) == 32, "TraceHeader size mismatch");

// 32-bank on-disk record (record_size == 32)
struct TraceRecord32 {
    int64_t  start;
    int64_t  end;
    uint32_t banks;
    uint16_t lhs_bw;
    uint16_t rhs_bw;
    uint8_t  kind;
    uint8_t  src;
    uint8_t  dst;
    uint8_t  pad[5];
};
static_assert(sizeof(TraceRecord32) == 32, "TraceRecord32 size mismatch");

// 128-bank on-disk record (record_size == 48)
struct TraceRecord128 {
    int64_t  start;
    int64_t  end;
    uint64_t banks[2];   // banks[b>>6] bit (b&63) = bank b active
    uint16_t lhs_bw;
    uint16_t rhs_bw;
    uint8_t  kind;
    uint8_t  src;
    uint8_t  dst;
    uint8_t  pad[9];
};
static_assert(sizeof(TraceRecord128) == 48, "TraceRecord128 size mismatch");

// Normalised record used internally after reading either format.
struct NormRecord {
    uint64_t banks[2];
    int      lhs_bw;
    int      rhs_bw;
    uint8_t  kind;
    uint8_t  src;
    uint8_t  dst;
};

enum OpKind : uint8_t { OP_MULI = 0, OP_ADDI = 1, OP_ROW_COPY = 2 };

const char*
kindName(uint8_t k)
{
    switch (k) {
      case OP_MULI:     return "MULI";
      case OP_ADDI:     return "ADDI";
      case OP_ROW_COPY: return "ROW_COPY";
      default:          return "UNKNOWN";
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

RowOpTracePlayer::RowOpTracePlayer(const Params &p)
    : TracePlayerBase(p),
      traceFile(p.trace_file),
      baseAddr(p.base_addr),
      banksPerChannel(p.banks_per_channel),
      channelSize(p.channel_size),
      rowStride(p.row_stride),
      lhsBase(SLOT_DATA_BASE), rhsBase(0), outBase(0),
      partialBase(0), tmpBase(0), carryBase(0),
      curMacro(-1),
      playerStats(this)
{
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

RowOpTracePlayer::RowOpTracePlayerStats::RowOpTracePlayerStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(numPacketsSent, statistics::units::Count::get(),
               "Total row-op packets sent to DRAM")
{
}

// ---------------------------------------------------------------------------
// Address helper
//
//   global_bank ∈ [0, TOTAL_BANKS)   (always 32, matches mimdram.h)
//   channel    = global_bank / banksPerChannel
//   local_bank = global_bank % banksPerChannel
//
//   slot_addr = base + channel * channelSize
//                    + (slot * banksPerChannel + local_bank) * ROW_SIZE
//
//   For DDR4 (banksPerChannel=32, channelSize unused):
//     channel=0, local_bank=global_bank → base + (slot*32+bank)*ROW_SIZE  ✓
//   For HBM2 (banksPerChannel=8, 4 channels):
//     bank 0-7 → ch0 at base; bank 8-15 → ch1 at base+channelSize; …
//   For HBM3 (banksPerChannel=16, 2 channels):
//     bank 0-15 → ch0 at base; bank 16-31 → ch1 at base+channelSize.
// ---------------------------------------------------------------------------

Addr
RowOpTracePlayer::slotAddr(int slot, int global_bank) const
{
    int channel    = global_bank / banksPerChannel;
    int local_bank = global_bank % banksPerChannel;
    return baseAddr
         + (Addr)channel * channelSize
         + ((Addr)slot * banksPerChannel + local_bank) * rowStride;
}

// ---------------------------------------------------------------------------
// Packet factory
// ---------------------------------------------------------------------------

PacketPtr
RowOpTracePlayer::makeRowOpPacket(Request::RowOp op,
                                   Addr dest, Addr src1, Addr src2)
{
    RequestPtr req = std::make_shared<Request>(dest,
                               sizeof(Request::RowOpPayload),
                               Request::UNCACHEABLE | Request::ROWOP,
                               requestorId);
    PacketPtr pkt = new Packet(req, MemCmd::WriteReq);

    // Allocate as uint8_t[] so Packet::deleteData()'s `delete [] data` is
    // well-defined (dataDynamic<T> stores the pointer as uint8_t* internally
    // and the destructor always uses array-delete).
    uint8_t* raw = new uint8_t[sizeof(Request::RowOpPayload)];
    Request::RowOpPayload payload = {op, dest, src1, src2, 0, 0, 0};
    memcpy(raw, &payload, sizeof(payload));
    pkt->dataDynamic(raw);

    return pkt;
}

// ---------------------------------------------------------------------------
// Primitive emitters
// ---------------------------------------------------------------------------

void RowOpTracePlayer::emitAAP(int dst_slot, int src_slot, int bank)
{
    pendingOps.push_back({Request::ROWAAP,
                          dst_slot, bank,
                          src_slot, bank,
                          0,        bank,
                          curMacro});
}

void RowOpTracePlayer::emitAP(int dst_slot, int bank)
{
    pendingOps.push_back({Request::ROWAP,
                          dst_slot, bank,
                          0, bank,
                          0, bank,
                          curMacro});
}

void RowOpTracePlayer::emitCopy(int dst_slot, int dst_bank,
                                 int src_slot, int src_bank)
{
    pendingOps.push_back({Request::ROWCOPY,
                          dst_slot, dst_bank,
                          src_slot, src_bank,
                          0, 0,
                          curMacro});
}

void RowOpTracePlayer::emitRdStream(int slot, int bank)
{
    pendingOps.push_back({Request::ROW_RD_STREAM,
                          slot, bank, slot, bank, 0, bank,
                          curMacro});
}

void RowOpTracePlayer::emitWrStream(int slot, int bank)
{
    pendingOps.push_back({Request::ROW_WR_STREAM,
                          slot, bank, slot, bank, 0, bank,
                          curMacro});
}

// ---------------------------------------------------------------------------
// Composite emitters (match C helpers in 93_simdram_schedule_runner.c)
// ---------------------------------------------------------------------------

// execute_row_and: T0←lhs, T1←rhs, T2←C_0, out←T0∧T1∧T2
void
RowOpTracePlayer::emitRowAnd(int lhs_slot, int rhs_slot, int out_slot,
                               const std::vector<int>& banks)
{
    for (int b : banks) emitAAP(SLOT_T0,       lhs_slot,    b);
    for (int b : banks) emitAAP(SLOT_T1,       rhs_slot,    b);
    for (int b : banks) emitAAP(SLOT_T2,       SLOT_C_0,    b);
    for (int b : banks) emitAAP(out_slot,      SLOT_T0_T1_T2, b);
}

// execute_row_add: full-adder, cin→cout stored in caller-supplied slots
void
RowOpTracePlayer::emitRowAdd(int lhs_slot, int rhs_slot, int out_slot,
                              int cin_slot, int cout_slot,
                              const std::vector<int>& banks)
{
    for (int b : banks) emitAAP(SLOT_DCC1,     cin_slot,       b);
    for (int b : banks) emitAAP(SLOT_T0_T1_T2, SLOT_DCC1,      b);
    for (int b : banks) emitAAP(SLOT_T2_T3,    lhs_slot,       b);
    for (int b : banks) emitAAP(SLOT_DCC1,     rhs_slot,       b);
    for (int b : banks) emitAAP(cout_slot,     SLOT_DCC1_T0_T3, b);
    for (int b : banks) emitAAP(SLOT_T0_T3,    SLOT_DCC1N,     b);
    for (int b : banks) emitAP (SLOT_T0_T1_T2,                  b);
    for (int b : banks) emitAAP(SLOT_T1,       rhs_slot,       b);
    for (int b : banks) emitAAP(out_slot,      SLOT_T1_T2_T3,  b);
}

// ---------------------------------------------------------------------------
// execute_add  (mirrors C code in 93_simdram_schedule_runner.c)
// ---------------------------------------------------------------------------
void
RowOpTracePlayer::expandAdd(int lhs_bw, int rhs_bw,
                             const std::vector<int>& banks)
{
    int bw = std::min(lhs_bw, rhs_bw);

    // init carry = 0 (DCC1 ← ~(DCC1 & C_0))
    for (int b : banks) emitAAP(SLOT_DCC1, SLOT_C_0, b);

    for (int j = 0; j < bw; j++) {
        int lhs_slot = lhsBase + j;
        int rhs_slot = rhsBase + j;
        int out_slot = outBase + j;

        for (int b : banks) emitAAP(SLOT_T0_T1_T2,  SLOT_DCC1,      b);
        for (int b : banks) emitAAP(SLOT_T2_T3,     lhs_slot,        b);
        for (int b : banks) emitAAP(SLOT_DCC1,      rhs_slot,        b);
        for (int b : banks) emitAP (SLOT_DCC1_T0_T3,                  b);
        for (int b : banks) emitAAP(SLOT_T0_T3,     SLOT_DCC1N,      b);
        for (int b : banks) emitAP (SLOT_T0_T1_T2,                    b);
        for (int b : banks) emitAAP(SLOT_T1,        rhs_slot,         b);
        for (int b : banks) emitAAP(out_slot,       SLOT_T1_T2_T3,   b);
    }
}

// ---------------------------------------------------------------------------
// execute_mul  (mirrors C code in 93_simdram_schedule_runner.c)
// ---------------------------------------------------------------------------
void
RowOpTracePlayer::expandMul(int lhs_bw, int rhs_bw,
                             const std::vector<int>& banks)
{
    // Phase 1: init out[0] and partial[] from rhs[0]
    emitRowAnd(lhsBase + 0, rhsBase + 0, outBase + 0, banks);

    for (int i = 0; i < lhs_bw - 1; i++)
        emitRowAnd(lhsBase + i + 1, rhsBase + 0, partialBase + i, banks);

    for (int b : banks) emitAAP(carryBase + 1, SLOT_C_0, b);

    // Phase 2: accumulate rhs[1..rhs_bw-2]
    for (int i = 0; i < rhs_bw - 1; i++) {
        emitRowAnd(lhsBase + 0, rhsBase + i, tmpBase + 0, banks);
        emitRowAdd(tmpBase + 0, partialBase + 0, outBase + i,
                   SLOT_C_0,    carryBase + 0,   banks);

        for (int j = 1; j < lhs_bw - 1; j++) {
            emitRowAnd(lhsBase + j, rhsBase + i, tmpBase + 0, banks);
            emitRowAdd(tmpBase + 0, partialBase + j, partialBase + j - 1,
                       carryBase + 0, carryBase + 0, banks);
        }

        emitRowAnd(lhsBase + lhs_bw - 1, rhsBase + i, tmpBase + 0, banks);
        emitRowAdd(tmpBase + 0, carryBase + 0, partialBase + lhs_bw - 2,
                   carryBase + 1, carryBase + 1, banks);
    }

    // Phase 3: final row rhs[rhs_bw-1]
    emitRowAnd(lhsBase + 0, rhsBase + rhs_bw - 1, tmpBase + 0, banks);

    for (int i = 1; i < lhs_bw - 1; i++) {
        emitRowAnd(lhsBase + i, rhsBase + rhs_bw - 1, tmpBase + 0, banks);
        emitRowAdd(tmpBase + 0, partialBase + i - 1, outBase + rhs_bw - 1 + i,
                   carryBase + 0, carryBase + 0, banks);
    }

    emitRowAnd(lhsBase + lhs_bw - 1, rhsBase + rhs_bw - 1, tmpBase + 0, banks);
    emitRowAdd(tmpBase + 0, carryBase + 1, outBase + lhs_bw + rhs_bw - 2,
               carryBase + 0, outBase + lhs_bw + rhs_bw - 1, banks);
}

// ---------------------------------------------------------------------------
// execute_row_copy_batch (single task: src_bank -> dst_bank for bw slots)
//
// Same-channel copy: emit a ROWCOPY packet handled entirely within one
// MemCtrl/AbstractMemory instance.
//
// Cross-channel copy: the ROWCOPY primitive in abstract_mem.cc accesses
// both dest and src1 relative to a single channel's pmemAddr base.  When
// the two banks are in different channels the src1 offset would go out of
// bounds and crash.  Model the inter-channel transfer as one full-row
// data-bus stream per side: ROW_RD_STREAM on the source channel (ACT +
// tRCD + columnsPerRowBuffer bursts out + tRTP + PRE) and ROW_WR_STREAM
// on the destination channel (same, with tWR recovery).  Each channel's
// data bus carries the row exactly once and the two sides pipeline like a
// host-buffered DMA (host interconnect bandwidth is assumed to not be the
// bottleneck).  This replaces the old 2-bare-ROWAP approximation, which
// charged no data movement at all and made a cross-channel copy cheaper
// than an intra-subarray RowClone FPM copy.
// ---------------------------------------------------------------------------
void
RowOpTracePlayer::expandRowCopy(int bw, int src_bank, int dst_bank)
{
    int src_channel = src_bank / banksPerChannel;
    int dst_channel = dst_bank / banksPerChannel;

    if (src_channel == dst_channel) {
        for (int j = 0; j < bw; j++)
            emitCopy(lhsBase + j, dst_bank, outBase + j, src_bank);
    } else {
        for (int j = 0; j < bw; j++) {
            emitRdStream(outBase + j, src_bank);  // src channel: row out
            emitWrStream(lhsBase + j, dst_bank);  // dst channel: row in
        }
    }
}

// ---------------------------------------------------------------------------
// loadTrace: two-pass over the binary file
//   Pass 1 – determine max bit-widths → assign slot offsets
//   Pass 2 – expand every record into PendingOps
// ---------------------------------------------------------------------------
void
RowOpTracePlayer::loadTrace()
{
    FILE* f = fopen(traceFile.c_str(), "rb");
    if (!f)
        panic("RowOpTracePlayer: cannot open trace file '%s'", traceFile.c_str());

    TraceHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        memcmp(hdr.magic, "CIMTRACE", 8) != 0 ||
        hdr.version != 1)
        panic("RowOpTracePlayer: invalid trace header in '%s'", traceFile.c_str());

    // Detect format from record_size; derive total bank count.
    int total_banks;
    if (hdr.record_size == sizeof(TraceRecord32)) {
        total_banks = 32;
    } else if (hdr.record_size == sizeof(TraceRecord128)) {
        total_banks = 128;
    } else {
        panic("RowOpTracePlayer: unrecognised record_size=%u in '%s' "
              "(expected 32 or 48)", hdr.record_size, traceFile.c_str());
    }

    // --- Read and normalise all records in one pass ---
    std::vector<NormRecord> records(hdr.num_records);
    int max_lhs = 0, max_rhs = 0, max_out = 0, max_partial = 0;

    for (uint64_t i = 0; i < hdr.num_records; i++) {
        NormRecord& nr = records[i];
        if (total_banks == 32) {
            TraceRecord32 raw;
            if (fread(&raw, sizeof(raw), 1, f) != 1)
                panic("RowOpTracePlayer: truncated trace at record %llu",
                      (unsigned long long)i);
            nr.banks[0] = raw.banks;
            nr.banks[1] = 0;
            nr.lhs_bw = raw.lhs_bw;
            nr.rhs_bw = raw.rhs_bw;
            nr.kind   = raw.kind;
            nr.src    = raw.src;
            nr.dst    = raw.dst;
        } else {
            TraceRecord128 raw;
            if (fread(&raw, sizeof(raw), 1, f) != 1)
                panic("RowOpTracePlayer: truncated trace at record %llu",
                      (unsigned long long)i);
            nr.banks[0] = raw.banks[0];
            nr.banks[1] = raw.banks[1];
            nr.lhs_bw = raw.lhs_bw;
            nr.rhs_bw = raw.rhs_bw;
            nr.kind   = raw.kind;
            nr.src    = raw.src;
            nr.dst    = raw.dst;
        }

        if (nr.kind == OP_MULI) {
            max_lhs     = std::max(max_lhs, nr.lhs_bw);
            max_rhs     = std::max(max_rhs, nr.rhs_bw);
            max_out     = std::max(max_out, nr.lhs_bw + nr.rhs_bw);
            max_partial = std::max(max_partial, nr.lhs_bw - 1);
        } else if (nr.kind == OP_ADDI) {
            max_lhs = std::max(max_lhs, nr.lhs_bw);
            max_rhs = std::max(max_rhs, nr.rhs_bw);
            max_out = std::max(max_out, std::min(nr.lhs_bw, nr.rhs_bw));
        } else { // ROW_COPY: src=out[], dst=lhs[]
            max_lhs = std::max(max_lhs, nr.lhs_bw);
            max_out = std::max(max_out, nr.lhs_bw);
        }
    }
    fclose(f);

    // --- Assign slot offsets ---
    //   18 control rows, then: lhs | rhs | out | partial | tmp(1) | carry(2)
    lhsBase     = SLOT_DATA_BASE;
    rhsBase     = lhsBase     + max_lhs;
    outBase     = rhsBase     + max_rhs;
    partialBase = outBase     + max_out;
    tmpBase     = partialBase + std::max(max_partial, 0);
    carryBase   = tmpBase     + 1;
    int totalSlots = carryBase + 2;

    inform("RowOpTracePlayer: %llu records, format=%d-bank, slots=%d (rows/bank)",
           (unsigned long long)hdr.num_records, total_banks, totalSlots);

    if (totalSlots >= 512)
        panic("RowOpTracePlayer: %d slots exceed rows_per_subarray (512)", totalSlots);

    // --- Exact total packet count from the closed form (no materialisation) ---
    // Validated to equal the eager expansion on every gemv/gemm trace.
    totalBanks_ = total_banks;
    totalPackets_ = 0;
    for (const NormRecord& nr : records) {
        Rec r{{nr.banks[0], nr.banks[1]}, nr.lhs_bw, nr.rhs_bw,
              nr.kind, nr.src, nr.dst};
        totalPackets_ += recordPackets(r);
    }

    // --- Lazy path: keep records, expand one at a time in makePacket() ---
    if (totalPackets_ > LAZY_THRESHOLD) {
        if (perChannel)
            panic("RowOpTracePlayer: trace expands to %llu packets (> %llu); "
                  "lazy streaming does not support --per-channel (its index "
                  "lists cannot hold that many entries).  Run single-port.",
                  (unsigned long long)totalPackets_,
                  (unsigned long long)LAZY_THRESHOLD);
        lazy_ = true;
        records_.reserve(records.size());
        for (const NormRecord& nr : records)
            records_.push_back({{nr.banks[0], nr.banks[1]}, nr.lhs_bw,
                                nr.rhs_bw, nr.kind, nr.src, nr.dst});
        inform("RowOpTracePlayer: %llu row-op packets across %llu records -- "
               "LAZY streaming (one record at a time, single-port; MacroOp and "
               "--per-channel disabled)",
               (unsigned long long)totalPackets_,
               (unsigned long long)records.size());
        return;
    }

    // --- Eager path: expand every record into PendingOps (unchanged) ---
    for (uint64_t i = 0; i < hdr.num_records; i++) {
        const NormRecord& r = records[i];

        // Build global bank list from 128-bit bitmask.
        std::vector<int> banks;
        for (int b = 0; b < total_banks; b++)
            if (r.banks[b >> 6] & (1ull << (b & 63)))
                banks.push_back(b);

        // Open a macro-op for this record; emit* tags every packet with it.
        // ROW_COPY carries its banks in src/dst rather than the bitmask.
        std::vector<int> macro_banks =
            (r.kind == OP_ROW_COPY) ? std::vector<int>{r.src, r.dst} : banks;

        curMacro = macroOps.size();
        macroOps.push_back({r.kind, i, r.lhs_bw, r.rhs_bw,
                            (int)macro_banks.size(),
                            r.src, r.dst, 0, 0, macro_banks});
        size_t before = pendingOps.size();

        switch (r.kind) {
          case OP_ADDI:
            expandAdd(r.lhs_bw, r.rhs_bw, banks);
            break;
          case OP_MULI:
            expandMul(r.lhs_bw, r.rhs_bw, banks);
            break;
          case OP_ROW_COPY:
            expandRowCopy(r.lhs_bw, r.src, r.dst);
            break;
          default:
            panic("RowOpTracePlayer: unknown op kind %u at record %llu",
                  r.kind, (unsigned long long)i);
        }

        macroOps[curMacro].npackets = pendingOps.size() - before;
    }
    curMacro = -1;

    // The closed-form count (used to pick eager vs lazy, and as numPackets() in
    // lazy mode) must agree with what the eager expansion actually produced.
    if (pendingOps.size() != totalPackets_)
        panic("RowOpTracePlayer: packet count mismatch -- closed form %llu vs "
              "eager %llu (recordPackets() formula is wrong)",
              (unsigned long long)totalPackets_,
              (unsigned long long)pendingOps.size());

    inform("RowOpTracePlayer: expanded to %llu row-op packets across "
           "%llu macro-ops",
           (unsigned long long)pendingOps.size(),
           (unsigned long long)macroOps.size());
}

// ---------------------------------------------------------------------------
// recordPackets: closed-form expansion size of one record (no materialisation).
// Mirrors the emit* sequences: every op-line is emitted once per active bank,
// so the per-bank line count is multiplied by the record's bank population.
// ---------------------------------------------------------------------------
size_t
RowOpTracePlayer::recordPackets(const Rec& r) const
{
    int nb = 0;
    for (int b = 0; b < totalBanks_; b++)
        if (r.banks[b >> 6] & (1ull << (b & 63)))
            nb++;

    switch (r.kind) {
      case OP_MULI: {
        // expandMul op-lines per bank: 4*rowAnds + 9*rowAdds + 1 carry-init,
        // with rowAnds = lhs*(rhs+1), rowAdds = (rhs-1)*lhs + (lhs-1).
        long lhs = r.lhs_bw, rhs = r.rhs_bw;
        size_t lines = 4 * (size_t)lhs * (rhs + 1)
                     + 9 * (size_t)((rhs - 1) * lhs + (lhs - 1)) + 1;
        return lines * (size_t)nb;
      }
      case OP_ADDI: {
        int bw = std::min(r.lhs_bw, r.rhs_bw);
        return (size_t)(8 * bw + 1) * (size_t)nb;
      }
      case OP_ROW_COPY: {
        // Same-channel: bw ROWCOPYs.  Cross-channel: bw rd-stream + bw wr-stream.
        bool xchan = (r.src / banksPerChannel) != (r.dst / banksPerChannel);
        return (size_t)(xchan ? 2 : 1) * (size_t)r.lhs_bw;
      }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// expandOneRecord: expand a single record into pendingOps (the lazy buffer).
// No macro-op is opened (MacroOp is disabled in lazy mode).
// ---------------------------------------------------------------------------
void
RowOpTracePlayer::expandOneRecord(uint64_t rec_idx)
{
    const Rec& r = records_[rec_idx];

    std::vector<int> banks;
    for (int b = 0; b < totalBanks_; b++)
        if (r.banks[b >> 6] & (1ull << (b & 63)))
            banks.push_back(b);

    pendingOps.clear();
    curMacro = -1;   // emit* tags packets with this; unused in lazy mode

    switch (r.kind) {
      case OP_ADDI:     expandAdd(r.lhs_bw, r.rhs_bw, banks); break;
      case OP_MULI:     expandMul(r.lhs_bw, r.rhs_bw, banks); break;
      case OP_ROW_COPY: expandRowCopy(r.lhs_bw, r.src, r.dst); break;
      default:
        panic("RowOpTracePlayer: unknown op kind %u at record %llu",
              r.kind, (unsigned long long)rec_idx);
    }
}

// ---------------------------------------------------------------------------
// makePacket: convert PendingOp[idx] to a row-op Packet (TracePlayerBase hook)
// ---------------------------------------------------------------------------

PacketPtr
RowOpTracePlayer::makePacket(size_t idx)
{
    if (lazy_) {
        // Forward cursor: advance the one-record buffer until it covers idx.
        // Empty records (0 packets) are skipped by the same loop.  The
        // single-port engine only ever asks for monotonically increasing idx,
        // so the buffer never has to seek backwards.
        while (idx >= bufBase_ + pendingOps.size()) {
            bufBase_ += pendingOps.size();
            expandOneRecord(curRec_++);
        }
        const PendingOp& op = pendingOps[idx - bufBase_];
        Addr dest = slotAddr(op.dest_slot, op.dest_bank);
        Addr src1 = slotAddr(op.src1_slot, op.src1_bank);
        Addr src2 = (op.op == Request::ROWCOPY) ? 0
                    : slotAddr(op.src2_slot, op.src2_bank);
        return makeRowOpPacket(op.op, dest, src1, src2);
    }

    const PendingOp& op = pendingOps[idx];

    Addr dest = slotAddr(op.dest_slot, op.dest_bank);
    Addr src1 = slotAddr(op.src1_slot, op.src1_bank);
    Addr src2 = (op.op == Request::ROWCOPY) ? 0
                                             : slotAddr(op.src2_slot, op.src2_bank);

    DPRINTF(RowOpTracePlayer,
            "Building op %llu/%llu: op=%d dest=0x%llx src1=0x%llx src2=0x%llx\n",
            (unsigned long long)idx, (unsigned long long)pendingOps.size(),
            (int)op.op, (unsigned long long)dest,
            (unsigned long long)src1, (unsigned long long)src2);

    return makeRowOpPacket(op.op, dest, src1, src2);
}

// ---------------------------------------------------------------------------
// onSent: per-packet counter, plus macro-op issue window under MacroOp.
//
// Counting sent packets (rather than keying off the macro's first/last index)
// keeps the START/END pair correct when the per-channel issue engine drains
// the sub-streams out of program order.
// ---------------------------------------------------------------------------
void
RowOpTracePlayer::onSent(size_t idx)
{
    playerStats.numPacketsSent++;

    // Lazy mode keeps no macro-op table (idx is a global index, not a buffer
    // offset), so MacroOp reporting is disabled there.
    if (lazy_)
        return;

    int m = pendingOps[idx].macro;
    if (m < 0)
        return;

    MacroOp& mo = macroOps[m];
    mo.sent++;

    if (mo.sent == 1) {
        // bank_list names every bank the record drives, so a Gantt can put
        // one lane per bank rather than only counting them.
        std::string bl;
        for (size_t i = 0; i < mo.banks.size(); i++) {
            if (i) bl += ',';
            bl += std::to_string(mo.banks[i]);
        }

        if (mo.kind == OP_ROW_COPY)
            DPRINTF(MacroOp,
                    "MacroOp START rec %llu %s bw %d src_bank %d dst_bank %d "
                    "banks %d bank_list %s packets %llu\n",
                    (unsigned long long)mo.rec, kindName(mo.kind), mo.lhs_bw,
                    mo.src, mo.dst, mo.nbanks, bl.c_str(),
                    (unsigned long long)mo.npackets);
        else
            DPRINTF(MacroOp,
                    "MacroOp START rec %llu %s lhs_bw %d rhs_bw %d banks %d "
                    "bank_list %s packets %llu\n",
                    (unsigned long long)mo.rec, kindName(mo.kind),
                    mo.lhs_bw, mo.rhs_bw, mo.nbanks, bl.c_str(),
                    (unsigned long long)mo.npackets);
    }

    if (mo.sent == mo.npackets)
        DPRINTF(MacroOp, "MacroOp END rec %llu %s packets %llu\n",
                (unsigned long long)mo.rec, kindName(mo.kind),
                (unsigned long long)mo.npackets);
}


} // namespace gem5