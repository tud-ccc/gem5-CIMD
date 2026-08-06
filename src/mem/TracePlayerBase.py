from m5.objects.ClockedObject import ClockedObject
from m5.params import *
from m5.proxy import *


# TracePlayerBase
#
# Abstract base for trace players that issue a precomputed sequence of packets
# against a MemCtrl.  Provides a pipelined send engine that mirrors OptiPIM's
# frontend: it pumps requests into the controller without waiting for each
# response, bounded only by issue_depth (outstanding requests) and
# issue_interval (spacing between issues).  Subclasses build the packet
# sequence and translate index -> PacketPtr.
class TracePlayerBase(ClockedObject):
    type = "TracePlayerBase"
    abstract = True
    cxx_header = "mem/trace_player_base.hh"
    cxx_class = "gem5::TracePlayerBase"

    port = RequestPort("Request port to send packets")

    # Per-channel issue path: one request port per channel, each with its own
    # retry slot, so a stall on one channel does not block issues to others
    # (removes the single-port head-of-line blocking).  Left unconnected in
    # the default single-port mode.
    chan_port = VectorRequestPort("Per-channel request ports")
    per_channel = Param.Bool(
        False,
        "Use the per-channel issue engine (one port + retry slot per channel)",
    )

    # Max outstanding (in-flight) requests.
    #   0 = unbounded: pump until the controller back-pressures
    #   1 = strictly serial (send -> wait response -> send next)
    #   N = keep up to N requests in flight
    issue_depth = Param.Unsigned(0, "Max outstanding requests (0 = unbounded)")

    # Minimum spacing between successive issues, in ticks (0 = no throttle:
    # issue as fast as the port accepts).  Set to ~tCK to emulate OptiPIM's
    # frontend injecting roughly one request per memory cycle.
    issue_interval = Param.Tick(0, "Min ticks between successive issues")

    system = Param.System(Parent.any, "System this player belongs to")
