#include "mem/nvt_trace_player.hh"

#include <cstring>
#include <fstream>
#include <sstream>

#include "base/trace.hh"
#include "debug/NvtTracePlayer.hh"
#include "mem/packet.hh"
#include "sim/system.hh"

namespace gem5
{

NvtTracePlayer::NvtTracePlayer(const Params &p)
    : TracePlayerBase(p),
      traceFile(p.trace_file),
      baseAddr(p.base_addr),
      traceRowSize(p.trace_row_size),
      banksPerChannel(p.banks_per_channel),
      rowStride(p.row_stride),
      bank(p.bank),
      compactRows(p.compact_rows),
      playerStats(this)
{
    if (traceRowSize == 0)
        fatal("NvtTracePlayer: trace_row_size must be non-zero");
    if (bank < 0 || bank >= banksPerChannel)
        fatal("NvtTracePlayer: bank %d out of range [0, %d)",
              bank, banksPerChannel);
}

NvtTracePlayer::NvtTracePlayerStats::NvtTracePlayerStats(
        statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(numAp, statistics::units::Count::get(),
               "Number of ROWAP (T, triple-row activate) packets sent"),
      ADD_STAT(numAap, statistics::units::Count::get(),
               "Number of ROWAAP (O, overlapped activate) packets sent")
{
}

// ---------------------------------------------------------------------------
// Address helper
//
// Under the RoRaBa* mappings consecutive row_stride steps walk banks, so a
// row index only advances the DRAM row after a full lap of the banks:
//
//   addr = base + (row * banks_per_channel + bank) * row_stride
// ---------------------------------------------------------------------------

Addr
NvtTracePlayer::slotAddr(uint32_t row) const
{
    return baseAddr + ((Addr)row * banksPerChannel + bank) * rowStride;
}

// ---------------------------------------------------------------------------
// Trace loading
//
//   <seq> T <row> <data> 0
//   <seq> O <src> <data> 0 <dst>
// ---------------------------------------------------------------------------

// Map a trace address to the DRAM row it replays into.  In compact mode
// each distinct address gets the next free row index, so N distinct rows
// occupy rows 0..N-1; otherwise the raw address/trace_row_size is used.
uint32_t
NvtTracePlayer::rowFor(Addr trace_addr)
{
    uint32_t raw = trace_addr / traceRowSize;
    if (!compactRows)
        return raw;

    auto it = rowIndex.find(trace_addr);
    if (it != rowIndex.end())
        return it->second;

    uint32_t next = rowIndex.size();
    rowIndex[trace_addr] = next;
    return next;
}

void
NvtTracePlayer::loadTrace()
{
    std::ifstream f(traceFile.c_str());
    if (!f.is_open())
        fatal("NvtTracePlayer: cannot open trace file '%s'", traceFile);

    std::string line;
    uint64_t line_no = 0;
    uint32_t max_row = 0;

    while (std::getline(f, line)) {
        line_no++;

        // skip empty / whitespace-only lines
        if (line.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;

        std::istringstream is(line);
        uint64_t    seq;
        std::string kind;
        std::string addr_tok, data_tok, flag_tok, dst_tok;

        if (!(is >> seq >> kind >> addr_tok >> data_tok >> flag_tok))
            fatal("NvtTracePlayer: malformed line %llu of '%s': %s",
                  line_no, traceFile, line);

        Addr addr = strtoull(addr_tok.c_str(), nullptr, 0);
        if (addr % traceRowSize != 0)
            fatal("NvtTracePlayer: address %#llx on line %llu is not a "
                  "multiple of trace_row_size (%llu)",
                  addr, line_no, (unsigned long long)traceRowSize);

        PendingOp op;
        if (kind == "T") {
            // Triple-row activate: one row operand, no source.
            op.op = Request::ROWAP;
            op.dest_row = rowFor(addr);
            op.src_row = op.dest_row;
        } else if (kind == "O") {
            // Overlapped activate: ACT src, ACT dst, PRE.
            if (!(is >> dst_tok))
                fatal("NvtTracePlayer: 'O' line %llu of '%s' is missing its "
                      "destination address: %s", line_no, traceFile, line);
            Addr dst = strtoull(dst_tok.c_str(), nullptr, 0);
            if (dst % traceRowSize != 0)
                fatal("NvtTracePlayer: destination %#llx on line %llu is not "
                      "a multiple of trace_row_size (%llu)",
                      dst, line_no, (unsigned long long)traceRowSize);
            op.op = Request::ROWAAP;
            op.src_row = rowFor(addr);
            op.dest_row = rowFor(dst);
        } else {
            fatal("NvtTracePlayer: unknown command '%s' on line %llu of "
                  "'%s' (expected T or O)", kind, line_no, traceFile);
        }

        max_row = std::max({max_row, op.dest_row, op.src_row});
        pendingOps.push_back(op);
    }

    f.close();

    inform("%s: loaded %llu row-ops from '%s' (%llu AAP, %llu AP), "
           "%u distinct rows, highest row index %u -> address %#llx",
           name().c_str(), (unsigned long long)pendingOps.size(),
           traceFile.c_str(),
           (unsigned long long)std::count_if(pendingOps.begin(),
                   pendingOps.end(),
                   [](const PendingOp& o)
                       { return o.op == Request::ROWAAP; }),
           (unsigned long long)std::count_if(pendingOps.begin(),
                   pendingOps.end(),
                   [](const PendingOp& o)
                       { return o.op == Request::ROWAP; }),
           compactRows ? (unsigned)rowIndex.size() : max_row + 1,
           max_row, (unsigned long long)slotAddr(max_row));
}

// ---------------------------------------------------------------------------
// Packet factory
// ---------------------------------------------------------------------------

PacketPtr
NvtTracePlayer::makeRowOpPacket(Request::RowOp op, Addr dest, Addr src1)
{
    RequestPtr req = std::make_shared<Request>(
            dest, sizeof(Request::RowOpPayload),
            Request::UNCACHEABLE | Request::ROWOP, requestorId);
    PacketPtr pkt = new Packet(req, MemCmd::WriteReq);

    // Allocate as uint8_t[] so Packet::deleteData()'s `delete [] data` is
    // well-defined (dataDynamic<T> stores the pointer as uint8_t* internally
    // and the destructor always uses array-delete).
    uint8_t* raw = new uint8_t[sizeof(Request::RowOpPayload)];
    // mask/size/n are zero: this is a timing-only replay of a precomputed
    // command schedule, so no operand bytes are moved.
    Request::RowOpPayload payload = {op, dest, src1, 0, 0, 0, 0};
    memcpy(raw, &payload, sizeof(payload));
    pkt->dataDynamic(raw);

    return pkt;
}

PacketPtr
NvtTracePlayer::makePacket(size_t idx)
{
    const PendingOp& op = pendingOps[idx];

    Addr dest = slotAddr(op.dest_row);
    Addr src1 = slotAddr(op.src_row);

    DPRINTF(NvtTracePlayer,
            "Building op %llu/%llu: %s dest=%#llx src=%#llx\n",
            (unsigned long long)idx, (unsigned long long)pendingOps.size(),
            op.op == Request::ROWAP ? "AP" : "AAP",
            (unsigned long long)dest, (unsigned long long)src1);

    return makeRowOpPacket(op.op, dest, src1);
}

void
NvtTracePlayer::onSent(size_t idx)
{
    if (pendingOps[idx].op == Request::ROWAP)
        playerStats.numAp++;
    else
        playerStats.numAap++;
}

} // namespace gem5
