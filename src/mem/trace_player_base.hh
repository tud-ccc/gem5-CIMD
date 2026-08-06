#ifndef __MEM_TRACE_PLAYER_BASE_HH__
#define __MEM_TRACE_PLAYER_BASE_HH__

#include <cstddef>
#include <string>
#include <vector>

#include "base/statistics.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/TracePlayerBase.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{

/**
 * TracePlayerBase
 *
 * Abstract base providing a pipelined send engine shared by the trace players.
 *
 * The engine mirrors OptiPIM's frontend: rather than waiting for each response
 * before issuing the next request, it keeps up to issueDepth requests in flight
 * (0 = unbounded -- pump until the controller back-pressures) and spaces
 * successive issues by issueInterval ticks (0 = no throttle).  The memory
 * controller's own FR-FCFS scheduler then overlaps commands across banks and
 * collapses row hits.
 *
 * Subclasses supply:
 *   buildTrace()        - populate the packet sequence (called from startup)
 *   numPackets()        - sequence length
 *   makePacket(idx)     - build the PacketPtr for sequence entry idx
 *   onSent(idx)         - hook to bump subclass-specific send counters
 */
class TracePlayerBase : public ClockedObject
{
  protected:

    // ------------------------------------------------------------------ //
    // Request port
    // ------------------------------------------------------------------ //
    class TraceRequestPort : public RequestPort
    {
      public:
        TraceRequestPort(const std::string& n, TracePlayerBase& p)
            : RequestPort(n), player(p) {}
      protected:
        bool recvTimingResp(PacketPtr pkt) override
            { return player.recvTimingResp(pkt); }
        void recvReqRetry() override { player.recvReqRetry(); }
        void recvTimingSnoopReq(PacketPtr) override {}
        void recvFunctionalSnoop(PacketPtr) override {}
        Tick recvAtomicSnoop(PacketPtr) override { return 0; }
      private:
        TracePlayerBase& player;
    };

    TraceRequestPort port;

    // ------------------------------------------------------------------ //
    // Per-channel request port: carries a channel id so retries and
    // responses route to the owning channel's independent state.
    // ------------------------------------------------------------------ //
    class ChanRequestPort : public RequestPort
    {
      public:
        ChanRequestPort(const std::string& n, TracePlayerBase& p, int ch)
            : RequestPort(n), player(p), chan(ch) {}
      protected:
        bool recvTimingResp(PacketPtr pkt) override
            { return player.recvTimingRespChan(chan, pkt); }
        void recvReqRetry() override { player.recvReqRetryChan(chan); }
        void recvTimingSnoopReq(PacketPtr) override {}
        void recvFunctionalSnoop(PacketPtr) override {}
        Tick recvAtomicSnoop(PacketPtr) override { return 0; }
      private:
        TracePlayerBase& player;
        int chan;
    };

    std::vector<ChanRequestPort*> chanPorts;

    // ------------------------------------------------------------------ //
    // Engine parameters / state
    // ------------------------------------------------------------------ //
    const bool     perChannel;     // use per-channel issue engine
    const unsigned issueDepth;     // max outstanding (0 = unbounded)
    const Tick     issueInterval;  // min ticks between issues (0 = none)
    RequestorID    requestorId;

    size_t    nextIdx;       // next sequence entry to send
    size_t    completed;     // responses received
    unsigned  outstanding;   // requests currently in flight
    PacketPtr retryPkt;      // packet awaiting retry (null if none)
    size_t    retryIdx;      // sequence index of retryPkt

    // ------------------------------------------------------------------ //
    // Per-channel engine state (used only when perChannel is true)
    // ------------------------------------------------------------------ //
    std::vector<std::vector<size_t>> chanQ;   // packet indices per channel
    std::vector<size_t>   chanCursor;         // next slot in chanQ[c]
    std::vector<unsigned> chanOutstanding;    // in-flight per channel
    std::vector<PacketPtr> chanRetry;         // retry pkt per channel

    // Optional dependency barriers (mirror of RowOpTracePlayer's start-group
    // engine): groupEnds[g] is the cumulative end index of issue group g.
    // Packets within a group are pumped concurrently; group g+1 starts only
    // after every packet of group g has retired.  Empty = no barriers
    // (legacy free-run).  Subclasses populate this from buildTrace().
    std::vector<size_t> groupEnds;
    size_t              curGroup;

    // ------------------------------------------------------------------ //
    // Engine internals
    // ------------------------------------------------------------------ //
    bool canIssue() const;
    bool tryIssueOne();   // true if a packet was accepted by the port
    void kick();          // schedule the next send opportunity if work remains
    void sendStep();
    EventFunctionWrapper sendEvent;

    // Port callbacks (single-port engine)
    bool recvTimingResp(PacketPtr pkt);
    void recvReqRetry();

    // Per-channel engine
    void partitionByChannel();      // build chanQ[] from packetChannel()
    void chanKick();                // schedule a pump if any channel has work
    void chanPumpAll();             // the per-channel send step
    bool recvTimingRespChan(int ch, PacketPtr pkt);
    void recvReqRetryChan(int ch);
    EventFunctionWrapper chanSendEvent;

    // ------------------------------------------------------------------ //
    // Subclass hooks
    // ------------------------------------------------------------------ //
    virtual void      buildTrace()            = 0;
    virtual size_t    numPackets() const      = 0;
    virtual PacketPtr makePacket(size_t idx)  = 0;
    virtual void      onSent(size_t idx)      { (void)idx; }
    // Channel a packet targets (per-channel engine).  Default 0 (single ch).
    virtual int       packetChannel(size_t idx) const { (void)idx; return 0; }

  public:
    PARAMS(TracePlayerBase);
    TracePlayerBase(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    void startup() override;

    // Shared statistic
    struct TracePlayerStats : public statistics::Group
    {
        TracePlayerStats(statistics::Group *parent);
        statistics::Scalar numRetries;
    } traceStats;
};

} // namespace gem5

#endif // __MEM_TRACE_PLAYER_BASE_HH__
