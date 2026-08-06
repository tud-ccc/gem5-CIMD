#include "mem/optipim_trace_player.hh"

#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/trace.hh"
#include "debug/MacroOp.hh"
#include "debug/OptiPimTracePlayer.hh"
#include "mem/packet.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5
{

// rows-per-bank used to pack/unpack the (bank, row) key in row_col_accesses.
// Must exceed the largest row offset any workload uses; HBM3_8Gb has
// 64 subarrays * 512 rows = 32768.  The exact value is irrelevant to the
// emitted gem5 address (which decodes bank and row separately) as long as
// encode and decode agree and row_offset < ROWS_PER_BANK.
static const int ROWS_PER_BANK = 32768;

namespace {

// Split s on every occurrence of delim (faithful to OptiPIM's tokenize()).
void tokenize(std::vector<std::string>& out, const std::string& s,
              const std::string& delim) {
    out.clear();
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(delim, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + delim.size();
    }
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

OptiPimTracePlayer::OptiPimTracePlayer(const Params &p)
    : TracePlayerBase(p),
      groupFile(p.group_file),
      baseAddr(p.base_addr),
      banksPerChannel(p.banks_per_channel),
      channelSize(p.channel_size),
      rowStride(p.row_stride),
      burstSize(p.burst_size),
      dq(p.dq),
      peBits(p.pe_bits),
      nCols(p.n_cols),
      bankMode(p.bank_mode == "single" ? BM_SINGLE
             : p.bank_mode == "serial" ? BM_SERIAL
                                        : BM_BROADCAST),
      spatialBanks(1),
      simTimesteps(p.sim_timesteps),
      fullReadout(p.full_readout),
      noLoads(p.no_loads),
      groupByTimestep(p.group_by_timestep),
      majModel(p.maj_model),
      rowsPerBank(ROWS_PER_BANK),
      readoutBanks(1),
      curMacro(0),
      macroStart(0),
      playerStats(this)
{
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

OptiPimTracePlayer::OptiPimTracePlayerStats::OptiPimTracePlayerStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(numRowAp, statistics::units::Count::get(),
               "Number of ROWAP row-op packets sent"),
      ADD_STAT(numRowAap, statistics::units::Count::get(),
               "Number of ROWAAP row-op packets sent"),
      ADD_STAT(numWrites, statistics::units::Count::get(),
               "Number of input-load write packets sent"),
      ADD_STAT(numReads, statistics::units::Count::get(),
               "Number of partial-sum read packets sent")
{
}

// ---------------------------------------------------------------------------
// Address helpers
// ---------------------------------------------------------------------------

int
OptiPimTracePlayer::addToBankTag(int row_offset, int global_bank_id) const
{
    return global_bank_id * rowsPerBank + row_offset;
}

// Reuse RowOpTracePlayer's per-channel slot scheme, plus a column byte offset
// within the row buffer for plain read/write.
Addr
OptiPimTracePlayer::slotAddr(int row, int global_bank, int col) const
{
    int channel    = global_bank / banksPerChannel;
    int local_bank = global_bank % banksPerChannel;
    int cols_per_row = (burstSize > 0) ? (int)(rowStride / burstSize) : 1;
    if (cols_per_row < 1) cols_per_row = 1;
    Addr col_off = (Addr)(col % cols_per_row) * (Addr)burstSize;
    return baseAddr
         + (Addr)channel * channelSize
         + ((Addr)row * banksPerChannel + local_bank) * rowStride
         + col_off;
}

// ---------------------------------------------------------------------------
// Packet factories
// ---------------------------------------------------------------------------

PacketPtr
OptiPimTracePlayer::makeRowApPacket(Addr dest)
{
    RequestPtr req = std::make_shared<Request>(dest,
                               sizeof(Request::RowOpPayload),
                               Request::UNCACHEABLE | Request::ROWOP,
                               requestorId);
    PacketPtr pkt = new Packet(req, MemCmd::WriteReq);

    uint8_t* raw = new uint8_t[sizeof(Request::RowOpPayload)];
    Request::RowOpPayload payload = {Request::ROWAP, dest, 0, 0, 0, 0, 0};
    memcpy(raw, &payload, sizeof(payload));
    pkt->dataDynamic(raw);
    return pkt;
}

PacketPtr
OptiPimTracePlayer::makeRowAapPacket(Addr dest, Addr src1)
{
    RequestPtr req = std::make_shared<Request>(dest,
                               sizeof(Request::RowOpPayload),
                               Request::UNCACHEABLE | Request::ROWOP,
                               requestorId);
    PacketPtr pkt = new Packet(req, MemCmd::WriteReq);

    uint8_t* raw = new uint8_t[sizeof(Request::RowOpPayload)];
    Request::RowOpPayload payload = {Request::ROWAAP, dest, src1, 0, 0, 0, 0};
    memcpy(raw, &payload, sizeof(payload));
    pkt->dataDynamic(raw);
    return pkt;
}

PacketPtr
OptiPimTracePlayer::makeMemPacket(Addr addr, bool isWrite)
{
    // Align to a burst so MemCtrl maps it to a single column access.
    Addr aligned = addr & ~((Addr)burstSize - 1);
    RequestPtr req = std::make_shared<Request>(aligned, burstSize,
                               Request::UNCACHEABLE, requestorId);
    PacketPtr pkt = new Packet(req,
                               isWrite ? MemCmd::WriteReq : MemCmd::ReadReq);
    pkt->allocate();
    return pkt;
}

// ---------------------------------------------------------------------------
// Group.txt parser  (faithful port of codegen_gemm / codegen_conv2d)
// ---------------------------------------------------------------------------

optipim::Layout*
OptiPimTracePlayer::parseGroup(const std::string& path)
{
    std::ifstream f(path.c_str());
    if (!f.is_open())
        panic("OptiPimTracePlayer: cannot open group file '%s'", path.c_str());

    std::string kernel;
    // tokens_list: each entry is {key, value} (or {kernel} for the first line)
    std::vector<std::vector<std::string>> tokens_list;

    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;
        if (t == "end") break;
        if (first) {
            kernel = t;
            first = false;
            continue;
        }
        size_t colon = t.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(t.substr(0, colon));
        std::string val = trim(t.substr(colon + 1));
        tokens_list.push_back({key, val});
    }
    f.close();

    // Shared field extraction
    std::vector<int> problem_dims;
    std::vector<std::string> loops;
    std::vector<char> ptags;
    std::vector<int> dims;
    std::vector<int> banks;
    std::vector<int> rows;
    std::map<std::string, std::vector<int>> coeffs;
    int Wdilation = 1, Hdilation = 1, Wstride = 1, Hstride = 1;

    for (auto& tok : tokens_list) {
        const std::string& key = tok[0];
        std::vector<std::string> array;
        tokenize(array, tok[1], ",");

        if (key.find("Problem") != std::string::npos) {
            for (auto& s : array) problem_dims.push_back(std::stoi(s));
        } else if (key.find("DilationStride") != std::string::npos) {
            Wdilation = std::stoi(array[0]);
            Hdilation = std::stoi(array[1]);
            Wstride   = std::stoi(array[2]);
            Hstride   = std::stoi(array[3]);
        } else if (key.find("Loops") != std::string::npos) {
            for (auto& s : array) loops.push_back(s);
        } else if (key.find("Bound") != std::string::npos) {
            for (auto& s : array) dims.push_back(std::stoi(s));
        } else if (key.find("Tag") != std::string::npos) {
            for (auto& s : array) ptags.push_back(s[0]);
        } else if (key.find("StartBankRow") != std::string::npos) {
            banks.push_back(std::stoi(array[0]));
            rows.push_back(std::stoi(array[1]));
        } else if (key.find("Coeff") != std::string::npos) {
            std::vector<int> coeffs_vec;
            for (auto& s : array) coeffs_vec.push_back(std::stoi(s));
            std::vector<std::string> tag_array;
            tokenize(tag_array, key, "_");
            std::string dim_tag(1, tag_array[1][0]);
            coeffs[dim_tag] = coeffs_vec;
        }
    }

    try {
        if (kernel == "conv2d" || kernel == "conv") {
            return new optipim::ConvLayout(problem_dims, Wdilation, Hdilation,
                                           Wstride, Hstride, loops, ptags, dims,
                                           coeffs, banks, rows, bankMode == BM_SINGLE);
        } else if (kernel == "gemm") {
            return new optipim::GemmLayout(problem_dims, loops, ptags, dims,
                                           coeffs, banks, rows, bankMode == BM_SINGLE);
        } else {
            panic("OptiPimTracePlayer: unsupported kernel '%s' in '%s'",
                  kernel.c_str(), path.c_str());
        }
    } catch (const std::exception& e) {
        panic("OptiPimTracePlayer: %s", e.what());
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// codegenBank  (faithful port of SimDRAMCodeGen::codegen_bank)
//
//   input load  (write) -> ordinary write packet
//   partial sum (read)  -> ordinary read packet
//   MAJ compute         -> ROWAP packet (one per of 7*pe_bits^2 + 1 ops)
// ---------------------------------------------------------------------------
uint32_t
OptiPimTracePlayer::beginMacro(MacroKind k, int bank, int row)
{
    curMacro   = macroOps.size();
    macroStart = pendingPkts.size();
    macroOps.push_back({k, bank, row, 0, 0});
    return curMacro;
}

void
OptiPimTracePlayer::endMacro(uint32_t id)
{
    // Drop phases that emitted nothing (weight tensors, or input loads under
    // --no-loads) so they never fire a START that has no END.
    size_t n = pendingPkts.size() - macroStart;
    if (n == 0)
        macroOps.pop_back();
    else
        macroOps[id].npackets = n;
}

void
OptiPimTracePlayer::codegenBank(
        int global_bank_id,
        const std::map<int, std::map<int, int>>& row_col_accesses,
        bool output_tensor, bool weight_tensor, bool first_time_in_col)
{
    (void)global_bank_id; // base addr derives from each row's source bank
    int n_rows_per_value = peBits;

    for (auto const& row : row_col_accesses) {
        int row_addr       = row.first % rowsPerBank;
        int source_bank_id = row.first / rowsPerBank;

        // --- Data loading ---
        // An output row reads its partial sums; any other row broadcasts its
        // input.  The two are mutually exclusive, so one macro-op covers the
        // whole data-movement phase of this row.
        uint32_t dm = beginMacro(output_tensor ? M_READOUT : M_LOAD,
                                 source_bank_id, row_addr);
        for (int bit_row = 0; bit_row < n_rows_per_value; bit_row++) {
            for (auto const& col : row.second) {
                if (weight_tensor) {
                    // Nothing for weights in SIMDRAM
                } else if (output_tensor) {
                    // Each spatial bank holds distinct partial sums; their
                    // read-out bursts share the channel bus, so under
                    // full_readout every bank's reads are emitted even when
                    // the ROWAP compute is collapsed to one representative.
                    for (int i = 0; i < readoutBanks; i++) {
                        Addr a = slotAddr(row_addr + bit_row,
                                          source_bank_id + i, col.first);
                        pendingPkts.push_back({K_READ, a, 0, dm});
                    }
                } else if (noLoads) {
                    // Input loads suppressed: compare compute + reduction
                    // read-out only (Cinnamon traces carry no input loads).
                } else {
                    // Input: broadcast write across col.second banks
                    if (first_time_in_col) {
                        for (int i = 0; i < col.second; i++) {
                            Addr a = slotAddr(0, source_bank_id + i, 0);
                            pendingPkts.push_back({K_WRITE, a, 0, dm});
                        }
                    }
                }
            }
        }
        endMacro(dm);

        // --- Computation: the per-output-value row-op burst ---
        if (output_tensor) {
            uint32_t cm = beginMacro(M_COMPUTE, source_bank_id, row_addr);
            emitComputeBurst(row_addr, source_bank_id, cm);
            endMacro(cm);
        }
    }
}

// ---------------------------------------------------------------------------
// Compute burst: the row-ops for ONE output value on one bank.
//
// Rows are cycled through the value's n_rows_per_value bit-rows so that every
// op is a row miss (an activate), matching OptiPIM's "alternating rows to
// emulate all rows are misses" comment.
// ---------------------------------------------------------------------------
void
OptiPimTracePlayer::emitComputeOp(Kind k, int row_addr, int first_bank,
                                  uint32_t macro, int& cur_row)
{
    int n = peBits;

    // The row indices advance once per OP, not per bank: every spatial bank
    // executes the identical row-op, on its own copy of the data.
    int dest_row = row_addr + cur_row;
    cur_row = (cur_row + 1) % n;

    // An AAP reads a source row and writes a dest row: take the source from
    // the next bit-row, so the two are distinct rows of this value and every
    // op remains a row miss.
    int src_row = -1;
    if (k == K_ROWAAP) {
        src_row = row_addr + cur_row;
        cur_row = (cur_row + 1) % n;
    }

    // Op-major / bank-minor: emit this one row-op to every spatial bank
    // back-to-back.  MemCtrl then sees N ACTs to distinct banks in a row and
    // paces them at tRRD under the tXAW cap -- i.e. the banks pipeline, which
    // is what an all-bank row-op costs on a shared CA bus.  Emitting bank-major
    // instead (BM_SERIAL) would drain one bank's whole chain first and the
    // banks would not overlap at all.
    int nbanks = (bankMode == BM_BROADCAST) ? spatialBanks : 1;
    for (int b = 0; b < nbanks; b++) {
        int bank  = first_bank + b;
        Addr dest = slotAddr(dest_row, bank, 0);
        if (k == K_ROWAAP)
            pendingPkts.push_back({K_ROWAAP, dest,
                                   slotAddr(src_row, bank, 0), macro});
        else
            pendingPkts.push_back({K_ROWAP, dest, 0, macro});
    }
}

// execute_row_and in RowOpTracePlayer: 4 AAPs.
void
OptiPimTracePlayer::emitCinRowAnd(int row_addr, int bank, uint32_t macro,
                                  int& cur_row)
{
    for (int i = 0; i < 4; i++)
        emitComputeOp(K_ROWAAP, row_addr, bank, macro, cur_row);
}

// execute_row_add in RowOpTracePlayer: 8 AAPs + 1 AP, the AP in position 7.
void
OptiPimTracePlayer::emitCinRowAdd(int row_addr, int bank, uint32_t macro,
                                  int& cur_row)
{
    for (int i = 0; i < 6; i++)
        emitComputeOp(K_ROWAAP, row_addr, bank, macro, cur_row);
    emitComputeOp(K_ROWAP,  row_addr, bank, macro, cur_row);   // ap T0_T1_T2
    for (int i = 0; i < 2; i++)
        emitComputeOp(K_ROWAAP, row_addr, bank, macro, cur_row);
}

void
OptiPimTracePlayer::emitComputeBurst(int row_addr, int first_bank,
                                     uint32_t macro)
{
    const int n = peBits;
    int cur_row = 0;

    if (majModel == "optipim") {
        // Legacy: OptiPIM's own MAJ count, every op a ROWAP.
        // int sequential_maj_ops = 7 * n * n + 1;   // pre-update formula
        int sequential_maj_ops = 12 * n * n - 12 * n + 4;
        for (int i = 0; i < sequential_maj_ops; i++)
            emitComputeOp(K_ROWAP, row_addr, first_bank, macro, cur_row);
        return;
    }

    // "cinnamon": the Ambit multiply expansion RowOpTracePlayer emits for a
    // MULI of lhs_bw = rhs_bw = n, op for op and in order (execute_mul).
    //   rowAnd = 4 AAP,  rowAdd = 8 AAP + 1 AP
    //   rowAnds = n^2 + n,  rowAdds = n^2 - 1,  plus 1 carry-init AAP
    //   => AAP = 12n^2 + 4n - 7,  AP = n^2 - 1,  total = 13n^2 + 4n - 8
    emitCinRowAnd(row_addr, first_bank, macro, cur_row);          // phase 1
    for (int i = 0; i < n - 1; i++)
        emitCinRowAnd(row_addr, first_bank, macro, cur_row);
    emitComputeOp(K_ROWAAP, row_addr, first_bank, macro, cur_row);// carry init

    for (int i = 0; i < n - 1; i++) {                                 // phase 2
        emitCinRowAnd(row_addr, first_bank, macro, cur_row);
        emitCinRowAdd(row_addr, first_bank, macro, cur_row);
        for (int j = 1; j < n - 1; j++) {
            emitCinRowAnd(row_addr, first_bank, macro, cur_row);
            emitCinRowAdd(row_addr, first_bank, macro, cur_row);
        }
        emitCinRowAnd(row_addr, first_bank, macro, cur_row);
        emitCinRowAdd(row_addr, first_bank, macro, cur_row);
    }

    emitCinRowAnd(row_addr, first_bank, macro, cur_row);          // phase 3
    for (int i = 1; i < n - 1; i++) {
        emitCinRowAnd(row_addr, first_bank, macro, cur_row);
        emitCinRowAdd(row_addr, first_bank, macro, cur_row);
    }
    emitCinRowAnd(row_addr, first_bank, macro, cur_row);
    emitCinRowAdd(row_addr, first_bank, macro, cur_row);
}

// ---------------------------------------------------------------------------
// interleaveByChannel: lockstep-merge a bank-major packet slice
// ---------------------------------------------------------------------------
void
OptiPimTracePlayer::interleaveByChannel(size_t from)
{
    std::vector<std::vector<PendingPkt>> perCh;
    for (size_t i = from; i < pendingPkts.size(); i++) {
        size_t ch = (pendingPkts[i].addr - baseAddr) / channelSize;
        if (ch >= perCh.size())
            perCh.resize(ch + 1);
        perCh[ch].push_back(pendingPkts[i]);
    }

    // Round-robin one packet per channel per turn; within a channel the
    // original (per-bank chain) order is preserved.
    size_t w = from;
    for (size_t r = 0; w < pendingPkts.size(); r++) {
        for (size_t c = 0; c < perCh.size(); c++) {
            if (r < perCh[c].size())
                pendingPkts[w++] = perCh[c][r];
        }
    }
}

// ---------------------------------------------------------------------------
// codegenSimdram  (faithful port of SimDRAMCodeGen::codegen_simdram)
// ---------------------------------------------------------------------------
void
OptiPimTracePlayer::codegenSimdram(optipim::Layout* layout)
{
    int n_values_per_dq  = dq;
    int n_values_per_row = nCols * n_values_per_dq;
    int input_load_banks = layout->m_spatial_banks;
    if (input_load_banks > banksPerChannel)
        input_load_banks = banksPerChannel;

    // Spatial banks of this layout, capped at what one channel holds -- the
    // row-op broadcast is a bank-internal operation within a channel.
    spatialBanks = input_load_banks;

    // Each spatial bank holds DISTINCT partial sums, so all of their read-outs
    // are real traffic that must cross the shared channel data bus.  Under
    // BM_BROADCAST (and BM_SERIAL) every bank's reads are emitted.  Only
    // BM_SINGLE collapses them, and then only if full_readout is off.
    if (bankMode == BM_BROADCAST)
        readoutBanks = input_load_banks;   // one codegenBank call covers all
                                           // banks, so it emits all their reads
    else if (bankMode == BM_SINGLE)
        readoutBanks = fullReadout ? input_load_banks : 1;
    else
        readoutBanks = 1;                  // BM_SERIAL walks each bank itself

    int temporal_steps  = layout->m_sequential_steps;
    int n_spatial_elems = layout->m_spatial_banks * layout->m_spatial_cols;
    int cols_per_bank   =
        int((n_spatial_elems - 1) / layout->m_spatial_banks + 1);
    int cur_row = 0, next_row = 0;

    int tid_step = 1;
    if (simTimesteps > 0)
        tid_step = (temporal_steps - 1) / simTimesteps + 1;

    // tensor_row_idx[i][sid] : hash -> row, for first-time-in-col detection
    std::vector<std::vector<std::map<int, int>>> tensor_row_idx;
    for (int i = 0; i < layout->n_tensors; i++)
        tensor_row_idx.push_back(
            std::vector<std::map<int, int>>(n_spatial_elems));

    int sim_timesteps = 0;
    for (int tid = 0; tid < temporal_steps; tid += tid_step) {
        sim_timesteps++;
        cur_row = next_row;
        layout->infer_layout(tid);

        for (int i = 0; i < layout->n_tensors; i++) {
            bool output_tensor = (i == layout->m_output_tensor);
            bool weight_tensor = (i == layout->m_weight_tensor);
            int prev_bank_id = 0;
            std::map<int, std::map<int, int>> row_col_accesses;
            int alloc_col = 0;
            bool first_time_in_col = false;

            for (int sid = 0; sid < n_spatial_elems; sid++) {
                int local_bank_id  = int(sid / cols_per_bank);
                int global_bank_id = local_bank_id + layout->m_banks[0];

                // BM_SINGLE / BM_BROADCAST both stop after the first bank's
                // accesses have been collected: BM_SINGLE then emits that one
                // bank and assumes the rest run free, while BM_BROADCAST emits
                // every row-op to all `spatialBanks` banks (op-major) so the
                // CA-bus cost of the broadcast is actually modelled.
                // BM_SERIAL falls through and walks every bank, bank-major.
                if (bankMode != BM_SERIAL && local_bank_id > 0) {
                    codegenBank(prev_bank_id, row_col_accesses,
                                output_tensor, weight_tensor, first_time_in_col);
                    row_col_accesses.clear();
                    break;
                }

                optipim::DataSpaceIdx tensor =
                    layout->m_pim_tensors[i][sid][0];
                int tensor_hash = tensor.get_hash(layout->m_bounds[i]);
                int tensor_offset = alloc_col;
                alloc_col++;
                int row_offset =
                    cur_row + (tensor_offset / n_values_per_row) * peBits;
                if (row_offset + 1 > next_row) next_row = row_offset;
                row_offset += layout->m_rows[0];
                int row_key = addToBankTag(row_offset, global_bank_id);
                int col_offset =
                    (tensor_offset % n_values_per_row) / n_values_per_dq;

                if (tensor_row_idx[i][sid].find(tensor_hash) ==
                    tensor_row_idx[i][sid].end()) {
                    first_time_in_col = true;
                    tensor_row_idx[i][sid][tensor_hash] = row_key;
                }

                if (global_bank_id != prev_bank_id ||
                    sid == n_spatial_elems - 1) {
                    codegenBank(prev_bank_id, row_col_accesses,
                                output_tensor, weight_tensor, first_time_in_col);
                    row_col_accesses.clear();
                } else {
                    row_col_accesses[row_key][col_offset] = input_load_banks;
                }
                prev_bank_id = global_bank_id;
            }
        }

        // One issue group per temporal step: banks within a step are
        // independent PUs (pumped concurrently), consecutive steps are a
        // true dependency (barrier).  The bank-major slice is lockstep-
        // merged across channels so the single-retry-slot pump does not
        // drain it one channel at a time.  Skip boundaries for empty steps.
        if (groupByTimestep &&
            (groupEnds.empty() || pendingPkts.size() > groupEnds.back())) {
            interleaveByChannel(groupEnds.empty() ? 0 : groupEnds.back());
            groupEnds.push_back(pendingPkts.size());
        }
    }

    inform("OptiPimTracePlayer: SimTimeSteps=%d, TotalSteps=%d, issueGroups=%d",
           sim_timesteps, temporal_steps,
           groupEnds.empty() ? 1 : (int)groupEnds.size());
}

// ---------------------------------------------------------------------------
// buildTrace: parse + codegen  (TracePlayerBase hook)
// ---------------------------------------------------------------------------
void
OptiPimTracePlayer::buildTrace()
{
    optipim::Layout* layout = parseGroup(groupFile);

    inform("OptiPimTracePlayer: kernel=%s spatial_banks=%d spatial_cols=%d "
           "sequential_steps=%d bank_mode=%s",
           layout->m_kernel.c_str(), layout->m_spatial_banks,
           layout->m_spatial_cols, layout->m_sequential_steps,
           bankMode == BM_SINGLE    ? "single"
         : bankMode == BM_SERIAL    ? "serial" : "broadcast");

    codegenSimdram(layout);
    delete layout;

    inform("OptiPimTracePlayer: generated %llu packets "
           "(ROWAP compute + read/write loads) across %llu macro-ops",
           (unsigned long long)pendingPkts.size(),
           (unsigned long long)macroOps.size());
}

// ---------------------------------------------------------------------------
// TracePlayerBase hooks: index -> PacketPtr, and per-kind send counters
// ---------------------------------------------------------------------------

PacketPtr
OptiPimTracePlayer::makePacket(size_t idx)
{
    const PendingPkt& pp = pendingPkts[idx];
    switch (pp.kind) {
      case K_ROWAP:  return makeRowApPacket(pp.addr);
      case K_ROWAAP: return makeRowAapPacket(pp.addr, pp.src1);
      case K_WRITE: return makeMemPacket(pp.addr, true);
      case K_READ:  return makeMemPacket(pp.addr, false);
      default:      panic("OptiPimTracePlayer: bad packet kind");
    }
    return nullptr;
}

namespace {

const char*
macroKindName(uint8_t k)
{
    switch (k) {
      case 0:  return "LOAD";      // M_LOAD
      case 1:  return "READOUT";   // M_READOUT
      case 2:  return "COMPUTE";   // M_COMPUTE
      default: return "UNKNOWN";
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// onSent: per-kind counters, plus the macro-op issue window under MacroOp.
//
// Counting sent packets (rather than keying off first/last index) keeps the
// START/END pair correct after interleaveByChannel has reordered the stream.
// ---------------------------------------------------------------------------
void
OptiPimTracePlayer::onSent(size_t idx)
{
    const PendingPkt& pp = pendingPkts[idx];

    switch (pp.kind) {
      case K_ROWAP:  playerStats.numRowAp++;  break;
      case K_ROWAAP: playerStats.numRowAap++; break;
      case K_WRITE:  playerStats.numWrites++; break;
      case K_READ:   playerStats.numReads++;  break;
    }

    MacroOp& mo = macroOps[pp.macro];
    mo.sent++;

    if (mo.sent == 1)
        DPRINTF(MacroOp,
                "MacroOp START id %llu %s bank %d row %d packets %llu\n",
                (unsigned long long)pp.macro, macroKindName(mo.kind),
                mo.bank, mo.row, (unsigned long long)mo.npackets);

    if (mo.sent == mo.npackets)
        DPRINTF(MacroOp, "MacroOp END id %llu %s bank %d row %d packets %llu\n",
                (unsigned long long)pp.macro, macroKindName(mo.kind),
                mo.bank, mo.row, (unsigned long long)mo.npackets);
}


} // namespace gem5