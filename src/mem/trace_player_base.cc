#include "mem/trace_player_base.hh"

#include <cassert>

#include "base/cprintf.hh"
#include "base/trace.hh"
#include "debug/TracePlayer.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5
{

TracePlayerBase::TracePlayerBase(const Params &p)
    : ClockedObject(p),
      port(name() + ".port", *this),
      perChannel(p.per_channel),
      issueDepth(p.issue_depth),
      issueInterval(p.issue_interval),
      requestorId(p.system->getRequestorId(this)),
      nextIdx(0),
      completed(0),
      outstanding(0),
      retryPkt(nullptr),
      retryIdx(0),
      curGroup(0),
      sendEvent([this]{ sendStep(); }, name()),
      chanSendEvent([this]{ chanPumpAll(); }, name()),
      traceStats(this)
{
    // One request port per connected channel (VectorRequestPort "chan_port").
    for (int i = 0; i < p.port_chan_port_connection_count; ++i) {
        chanPorts.push_back(
            new ChanRequestPort(csprintf("%s.chan_port[%d]", name(), i),
                                *this, i));
    }
}

Port &
TracePlayerBase::getPort(const std::string& if_name, PortID idx)
{
    if (if_name == "port")
        return port;
    if (if_name == "chan_port" && idx < chanPorts.size())
        return *chanPorts[idx];
    return ClockedObject::getPort(if_name, idx);
}

TracePlayerBase::TracePlayerStats::TracePlayerStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(numRetries, statistics::units::Count::get(),
               "Number of send retries due to back-pressure")
{
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

bool
TracePlayerBase::canIssue() const
{
    if (nextIdx >= numPackets()) return false;   // nothing left to send
    if (retryPkt != nullptr)     return false;   // blocked on a retry
    if (issueDepth != 0 && outstanding >= issueDepth) return false; // depth full
    // Dependency barrier: do not cross into the next issue group until the
    // current one has fully retired (checked in recvTimingResp).
    if (!groupEnds.empty() && nextIdx >= groupEnds[curGroup]) return false;
    return true;
}

bool
TracePlayerBase::tryIssueOne()
{
    PacketPtr pkt = makePacket(nextIdx);

    DPRINTF(TracePlayer, "Issuing pkt %llu/%llu (outstanding=%u)\n",
            (unsigned long long)nextIdx,
            (unsigned long long)numPackets(), outstanding);

    if (!port.sendTimingReq(pkt)) {
        // Back-pressure: stash and wait for recvReqRetry.
        retryPkt = pkt;
        retryIdx = nextIdx;
        traceStats.numRetries++;
        return false;
    }

    onSent(nextIdx);
    nextIdx++;
    outstanding++;
    return true;
}

void
TracePlayerBase::kick()
{
    if (!sendEvent.scheduled() && canIssue()) {
        Tick when = curTick() + (issueInterval > 0 ? issueInterval : 1);
        schedule(sendEvent, when);
    }
}

void
TracePlayerBase::sendStep()
{
    if (issueInterval == 0) {
        // No throttle: pump as many as allowed until depth/back-pressure/done.
        while (canIssue() && tryIssueOne()) { }
    } else {
        // Throttled: at most one issue per interval.
        if (canIssue()) tryIssueOne();
    }
    kick();
}

bool
TracePlayerBase::recvTimingResp(PacketPtr pkt)
{
    // The Request is refcounted (RequestPtr) and released with the packet.
    delete pkt;

    assert(outstanding > 0);
    outstanding--;
    completed++;

    if (completed >= numPackets()) {
        inform("%s: all %llu packets completed, exiting",
               name().c_str(), (unsigned long long)numPackets());
        exitSimLoop(name() + ": trace replay complete");
        return true;
    }

    // Barrier release: advance past every group whose packets have all
    // retired (empty groups collapse in the same pass).
    if (!groupEnds.empty()) {
        while (curGroup + 1 < groupEnds.size() &&
               completed >= groupEnds[curGroup]) {
            curGroup++;
        }
    }

    // A slot may have freed up (depth-bounded); resume issuing.
    kick();
    return true;
}

void
TracePlayerBase::recvReqRetry()
{
    assert(retryPkt != nullptr);

    if (!port.sendTimingReq(retryPkt)) {
        traceStats.numRetries++;
        return; // still blocked; wait for the next retry
    }

    onSent(retryIdx);
    retryPkt = nullptr;
    nextIdx++;
    outstanding++;
    kick();
}

// ---------------------------------------------------------------------------
// startup
// ---------------------------------------------------------------------------
void
TracePlayerBase::startup()
{
    buildTrace();
    if (numPackets() == 0)
        return;

    if (perChannel) {
        if (!groupEnds.empty())
            panic("TracePlayerBase: per-channel engine does not support "
                  "group-by-timestep barriers");
        if (chanPorts.empty())
            panic("TracePlayerBase: per_channel set but no chan_port "
                  "connected");
        partitionByChannel();
        schedule(chanSendEvent, curTick());
    } else {
        schedule(sendEvent, curTick());
    }
}

// ===========================================================================
// Per-channel issue engine
//
// One request port per channel, each with its own retry slot, draining its own
// in-order sub-stream.  A stall on channel c parks chanRetry[c] but leaves the
// other channels free to keep issuing -- removing the single-port head-of-line
// blocking.  Packets keep their original per-channel order (correctness); only
// cross-channel ordering is relaxed, which the DRAM has no dependency on.
// ===========================================================================
void
TracePlayerBase::partitionByChannel()
{
    size_t n = chanPorts.size();
    chanQ.assign(n, {});
    chanCursor.assign(n, 0);
    chanOutstanding.assign(n, 0);
    chanRetry.assign(n, nullptr);

    for (size_t i = 0; i < numPackets(); ++i) {
        int c = packetChannel(i);
        if (c < 0 || (size_t)c >= n)
            panic("TracePlayerBase: packet %llu maps to channel %d, out of "
                  "range [0,%llu)", (unsigned long long)i, c,
                  (unsigned long long)n);
        chanQ[c].push_back(i);
    }
    inform("%s: per-channel engine, %llu channels", name().c_str(),
           (unsigned long long)n);
}

void
TracePlayerBase::chanKick()
{
    if (chanSendEvent.scheduled())
        return;
    // Any channel with work left and not blocked on a retry?
    for (size_t c = 0; c < chanPorts.size(); ++c) {
        bool depth_ok = (issueDepth == 0 || chanOutstanding[c] < issueDepth);
        if (chanRetry[c] == nullptr && chanCursor[c] < chanQ[c].size() &&
            depth_ok) {
            Tick when = curTick() + (issueInterval > 0 ? issueInterval : 1);
            schedule(chanSendEvent, when);
            return;
        }
    }
}

void
TracePlayerBase::chanPumpAll()
{
    // Pump every channel independently, one round of as many as each accepts.
    for (size_t c = 0; c < chanPorts.size(); ++c) {
        while (chanRetry[c] == nullptr &&
               chanCursor[c] < chanQ[c].size() &&
               (issueDepth == 0 || chanOutstanding[c] < issueDepth)) {
            size_t idx = chanQ[c][chanCursor[c]];
            PacketPtr pkt = makePacket(idx);
            if (!chanPorts[c]->sendTimingReq(pkt)) {
                chanRetry[c] = pkt;
                traceStats.numRetries++;
                break;               // this channel stalls; others continue
            }
            onSent(idx);
            chanCursor[c]++;
            chanOutstanding[c]++;
            if (issueInterval > 0) break;   // throttle: one per interval per ch
        }
    }
    chanKick();
}

bool
TracePlayerBase::recvTimingRespChan(int ch, PacketPtr pkt)
{
    delete pkt;

    assert(chanOutstanding[ch] > 0);
    chanOutstanding[ch]--;
    completed++;

    if (completed >= numPackets()) {
        inform("%s: all %llu packets completed, exiting",
               name().c_str(), (unsigned long long)numPackets());
        exitSimLoop(name() + ": trace replay complete");
        return true;
    }
    chanKick();
    return true;
}

void
TracePlayerBase::recvReqRetryChan(int ch)
{
    assert(chanRetry[ch] != nullptr);
    if (!chanPorts[ch]->sendTimingReq(chanRetry[ch])) {
        traceStats.numRetries++;
        return; // still blocked on this channel
    }
    onSent(chanQ[ch][chanCursor[ch]]);
    chanRetry[ch] = nullptr;
    chanCursor[ch]++;
    chanOutstanding[ch]++;
    chanKick();
}

} // namespace gem5
