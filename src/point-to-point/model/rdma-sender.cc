#include "rdma-sender.h"

#include "rdma-receiver.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/enum.h"
#include "ns3/hash.h"
#include "ns3/ipv6-header.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/udp-header.h"
#include "ns3/uinteger.h"

#include <algorithm>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RdmaSender");

NS_OBJECT_ENSURE_REGISTERED(RdmaSender);

RdmaSender::GlobalStats RdmaSender::s_stats;

namespace
{
const uint8_t IPV6_NEXT_HEADER_UDP = 17;
const uint32_t FLOW_LABEL_MASK = 0xFFFFF;

const double RTT_EWMA_GAIN = 1.0 / 8;   //!< avg_rtt += gain * (sample - avg_rtt)
const double MD_GAIN = 0.8;             //!< Multiplicative decrease strength
const double MD_MIN_FACTOR = 0.5;       //!< At most halve the window per decrease
const double FAST_INCREASE_SLACK = 0.25; //!< "RTT ~ base": within 1/4 of (target - base)
const double MAX_REORDER_WINDOW_RTTS = 16; //!< Cap of the adaptive reorder window
const uint32_t INITIAL_WINDOW_RING = 64;   //!< Tracked PSNs before the ring grows
} // namespace

TypeId
RdmaSender::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::RdmaSender")
            .SetParent<Object>()
            .SetGroupName("PointToPoint")
            .AddAttribute("MtuShift",
                          "log2 of the payload bytes per packet: 8..12 (RoCE path MTU 256..4096)",
                          UintegerValue(12),
                          MakeUintegerAccessor(&RdmaSender::m_mtuShift),
                          MakeUintegerChecker<uint32_t>(8, 12))
            .AddAttribute("RttSerializationBytes",
                          "Bytes of serialization at the first plane's rate added to the QP's "
                          "base (propagation) RTT for the initial base RTT estimate",
                          UintegerValue(1000),
                          MakeUintegerAccessor(&RdmaSender::m_rttSerializationBytes),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("Mpr",
                          "Maximum PSN Range: outstanding packets (must match the receiver)",
                          UintegerValue(1024),
                          MakeUintegerAccessor(&RdmaSender::m_mpr),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("ReorderWindow",
                          "Time-based loss detection: a packet is lost once a later-sent packet "
                          "was acknowledged and it has been outstanding for the smoothed RTT plus "
                          "this many base RTTs (doubled on each spurious detection, up to 16). "
                          "8 absorbs the reordering of spraying over full buffers",
                          DoubleValue(8.0),
                          MakeDoubleAccessor(&RdmaSender::m_reorderWindow),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("MinRto",
                          "Initial retransmission timeout; must exceed the worst queueing "
                          "delay (incl. PFC pauses) to avoid spurious timeouts",
                          TimeValue(MicroSeconds(400)),
                          MakeTimeAccessor(&RdmaSender::m_minRto),
                          MakeTimeChecker())
            .AddAttribute("MaxRto",
                          "Retransmission timeout cap for exponential backoff",
                          TimeValue(MilliSeconds(2)),
                          MakeTimeAccessor(&RdmaSender::m_maxRto),
                          MakeTimeChecker())
            .AddAttribute("PacketSpraying",
                          "Use a new IPv6 flow label (entropy value) for every packet",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RdmaSender::m_packetSpraying),
                          MakeBooleanChecker())
            .AddAttribute("UseTimestamp",
                          "Carry a TSETH timestamp, echoed back in CC_STATE for RTT samples "
                          "(otherwise RTT is measured from local send times)",
                          BooleanValue(false),
                          MakeBooleanAccessor(&RdmaSender::m_useTimestamp),
                          MakeBooleanChecker())
            .AddAttribute("TargetRttFactor",
                          "NSCC target RTT as a multiple of the base RTT",
                          DoubleValue(1.5),
                          MakeDoubleAccessor(&RdmaSender::m_targetRttFactor),
                          MakeDoubleChecker<double>(1.0))
            .AddAttribute("MaxCwndBdp",
                          "NSCC window cap in BDPs (line rate x base RTT)",
                          DoubleValue(1.5),
                          MakeDoubleAccessor(&RdmaSender::m_maxCwndBdp),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("FairIncrease",
                          "NSCC fair increase, scaled by the BDP: fraction of a BDP added per "
                          "window of acknowledged bytes",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&RdmaSender::m_fairIncrease),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("FastIncrease",
                          "NSCC Fast Increase: MTUs added per ack once a window went "
                          "uncongested at base RTT",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RdmaSender::m_fastIncrease),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("EcmpEvSetSize",
                          "Hash-based ECMP (no SourceRouting) with PacketSpraying: rotate over "
                          "a fixed set of this many random EVs (flow labels) per QP, as MRC "
                          "does with ECMP; 0: a fresh EV for every packet",
                          UintegerValue(0),
                          MakeUintegerAccessor(&RdmaSender::m_ecmpEvSetSize),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("EvOnEcn",
                          "SourceRouting: an ECN mark on a SACK moves its EV to the backup queue",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RdmaSender::m_evOnEcn),
                          MakeBooleanChecker())
            .AddAttribute("EvOnLoss",
                          "SourceRouting: a trim NACK, or an RTO on the oldest outstanding "
                          "packet, moves its EV to the backup queue",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RdmaSender::m_evOnLoss),
                          MakeBooleanChecker())
            .AddAttribute("SourceRouting",
                          "MRC static SRv6 source routing with EV management: every EV maps "
                          "to a path given by AddPath(); an ECN mark, trim or RTO moves an EV to the backup queue",
                          BooleanValue(false),
                          MakeBooleanAccessor(&RdmaSender::m_sourceRouting),
                          MakeBooleanChecker())
            .AddAttribute("EvsPerPlane",
                          "SourceRouting: active EVs (random paths) per plane, the other paths "
                          "are backups; 0: half of the plane's paths (rounded up); more than the "
                          "plane has: all of them",
                          UintegerValue(0),
                          MakeUintegerAccessor(&RdmaSender::m_evsPerPlane),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("LoadBalancing",
                          "How EVs (paths) are chosen: Mrc (ECMP / spraying, or SRv6 active "
                          "and backup EV sets) or Reps (Recycled Entropy Packet Spraying)",
                          EnumValue(LB_MRC),
                          MakeEnumAccessor<LoadBalancing>(&RdmaSender::m_loadBalancing),
                          MakeEnumChecker(LB_MRC, "Mrc", LB_REPS, "Reps"))
            .AddAttribute("RepsBufferSize",
                          "REPS: cached EVs per plane (circular buffer)",
                          UintegerValue(8),
                          MakeUintegerAccessor(&RdmaSender::m_repsBufferSize),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("RepsExplorePackets",
                          "REPS: packets per plane of the exploration phase (new or idle "
                          "connection); 0: one BDP per plane",
                          UintegerValue(0),
                          MakeUintegerAccessor(&RdmaSender::m_repsExplorePackets),
                          MakeUintegerChecker<uint32_t>());
    return tid;
}

RdmaSender::RdmaSender(QpInfo qp)
    : m_qp(qp),
      m_mtuShift(12),
      m_rttSerializationBytes(1000),
      m_mpr(1024),
      m_reorderWindow(8.0),
      m_minRto(MicroSeconds(400)),
      m_maxRto(MilliSeconds(2)),
      m_packetSpraying(true),
      m_useTimestamp(false),
      m_targetRttFactor(1.5),
      m_maxCwndBdp(1.5),
      m_fairIncrease(0.5),
      m_fastIncrease(2),
      m_sourceRouting(false),
      m_evOnEcn(true),
      m_evOnLoss(true),
      m_evsPerPlane(0),
      m_loadBalancing(LB_MRC),
      m_repsBufferSize(8),
      m_repsExplorePackets(0),
      m_evRng(qp.id),
      m_ecmpEvSetSize(0)
{
    NS_ABORT_MSG_IF(qp.id > BthHeader::QPN_MASK, "QP " << qp.id << " exceeds 24-bit QPN");
}

RdmaSender::~RdmaSender()
{
}

void
RdmaSender::NotifyConstructionCompleted()
{
    Object::NotifyConstructionCompleted();

    // Attributes are applied now: size everything that depends on them
    m_mtu = 1u << m_mtuShift;
    m_window.assign(std::min(m_mpr, INITIAL_WINDOW_RING), PacketInfo());
    m_rto = m_minRto;
    m_reorderWindowRtts = m_reorderWindow;
    m_srcAddress = GetHostAddress(m_qp.src);
    m_dstAddress = GetHostAddress(m_qp.dst);
    m_entropy = NextEntropy();
    for (uint32_t i = 0; !m_sourceRouting && i < m_ecmpEvSetSize; i++)
    {
        m_ecmpEvs.push_back(m_evRng() & FLOW_LABEL_MASK);
    }
}

void
RdmaSender::DoDispose()
{
    Simulator::Cancel(m_rtoEvent);
    m_planeWakeups.clear();
    m_onMessageAcked = Callback<void, uint32_t>();
    Object::DoDispose();
}

Ipv6Address
RdmaSender::GetHostAddress(uint32_t hostId)
{
    uint8_t buf[16] = {0xfd, 0x00};
    buf[12] = hostId >> 24;
    buf[13] = hostId >> 16;
    buf[14] = hostId >> 8;
    buf[15] = hostId;
    return Ipv6Address(buf);
}

void
RdmaSender::PostMessage(uint32_t msgId, uint32_t size)
{
    uint32_t numPackets = std::max<uint32_t>(1, (uint64_t(size) + m_mtu - 1) >> m_mtuShift);
    NS_ABORT_MSG_IF(uint64_t(m_postedPsn) + numPackets > UINT32_MAX,
                    "QP " << m_qp.id << ": PSN space exhausted");
    bool wasBlocked = !HasDataToSend();
    if (IsIdle())
    {
        for (RepsState& reps : m_reps)
        {
            reps.explorePending = true; // REPS explores again after an idle period
        }
    }
    m_messages.push_back({msgId, size, m_nextMsn++, m_postedPsn, numPackets});
    m_postedPsn += numPackets;
    if (wasBlocked && HasDataToSend())
    {
        NotifyPlanes();
    }
}

const RdmaSender::Message&
RdmaSender::MessageOf(uint32_t psn) const
{
    // Messages are in PSN order: the last one starting at or before psn
    auto it = std::upper_bound(m_messages.begin(),
                               m_messages.end(),
                               psn,
                               [](uint32_t p, const Message& m) { return p < m.firstPsn; });
    NS_ASSERT_MSG(it != m_messages.begin(), "PSN " << psn << " of no posted message");
    return *std::prev(it);
}

void
RdmaSender::GrowWindow()
{
    if (m_nextPsn - m_cumAck < m_window.size() || m_window.size() >= m_mpr)
    {
        return;
    }
    std::vector<PacketInfo> window(std::min<size_t>(m_mpr, 2 * m_window.size()));
    for (uint32_t psn = m_cumAck; psn < m_nextPsn; psn++)
    {
        window[psn % window.size()] = Info(psn);
    }
    m_window.swap(window);
}

void
RdmaSender::AddPath(uint32_t plane, const Srv6Sid& path)
{
    if (m_planePaths.size() <= plane)
    {
        m_planePaths.resize(plane + 1);
    }
    EntropyValue ev;
    ev.plane = plane;
    ev.path = path;
    m_planePaths[plane].push_back(m_evs.size());
    m_evs.push_back(ev);
    NS_ABORT_MSG_IF(m_evs.size() > FLOW_LABEL_MASK + 1, "Too many EVs for the flow label");
}

uint32_t
RdmaSender::AddPlane(DataRate rate, Callback<void> wakeup)
{
    uint32_t plane = m_planeWakeups.size();
    m_planeWakeups.push_back(wakeup);

    // The plane's own controller, starting at its line rate
    PlaneCc cc;
    cc.rate = rate;
    cc.baseRtt = m_qp.minRttNs + m_rttSerializationBytes * 8 * 1e9 / rate.GetBitRate();
    UpdateLimits(cc);
    cc.cwnd = cc.bdp;
    ClampCwnd(cc);
    m_cc.push_back(cc);

    if (m_loadBalancing == LB_REPS)
    {
        InitReps(plane);
    }
    else if (m_sourceRouting)
    {
        BuildEvSet(plane);
    }
    return plane;
}

void
RdmaSender::BuildEvSet(uint32_t plane)
{
    NS_ABORT_MSG_IF(plane >= m_planePaths.size() || m_planePaths[plane].empty(),
                    "QP " << m_qp.id << ": SourceRouting needs AddPath() for plane " << plane
                            << " before AddPlane()");
    m_activeEvs.resize(plane + 1);
    m_backupEvs.resize(plane + 1);
    m_evCursor.resize(plane + 1, 0);
    m_evTarget.resize(plane + 1, 0);

    // Same number of active EVs on every plane (keeps planes equally loaded),
    // sized by the plane with the fewest paths; which paths is random within
    // the plane and senders do not coordinate
    size_t fewestPaths = m_planePaths[plane].size();
    for (const auto& paths : m_planePaths)
    {
        fewestPaths = std::min(fewestPaths, paths.size());
    }
    std::vector<uint32_t> evs = m_planePaths[plane];
    std::shuffle(evs.begin(), evs.end(), m_evRng);
    uint32_t active = m_evsPerPlane ? std::min<uint32_t>(m_evsPerPlane, fewestPaths)
                                    : (fewestPaths + 1) / 2;
    // Signals may shrink the active set down to half of the paths, never below
    m_evTarget[plane] = std::min<uint32_t>(active, (fewestPaths + 1) / 2);
    m_activeEvs[plane].assign(evs.begin(), evs.begin() + active);
    m_backupEvs[plane].assign(evs.begin() + active, evs.end());
}

int32_t
RdmaSender::SelectEv(uint32_t plane)
{
    std::vector<uint32_t>& active = m_activeEvs[plane];
    if (active.empty())
    {
        return -1;
    }
    // Spray over every active EV in turn
    uint32_t& cursor = m_evCursor[plane];
    uint32_t ev = active[cursor % active.size()];
    cursor = (cursor + 1) % active.size();
    return ev;
}

void
RdmaSender::OnEvSignal(int32_t ev)
{
    if (m_loadBalancing != LB_MRC || ev < 0 || uint32_t(ev) >= m_evs.size())
    {
        return;
    }
    uint32_t plane = m_evs[ev].plane;
    std::vector<uint32_t>& active = m_activeEvs[plane];
    auto it = std::find(active.begin(), active.end(), uint32_t(ev));
    if (it == active.end())
    {
        return; // already a backup
    }
    // To the back of the backup queue
    std::deque<uint32_t>& backups = m_backupEvs[plane];
    active.erase(it);
    backups.push_back(ev);
    s_stats.evSwaps++;

    // Below the target (half of the plane's paths): the front of the queue rejoins
    while (active.size() < m_evTarget[plane] && !backups.empty())
    {
        active.push_back(backups.front());
        backups.pop_front();
        s_stats.evRejoins++;
    }
    m_evCursor[plane] = active.empty() ? 0 : m_evCursor[plane] % active.size();
}

void
RdmaSender::InitReps(uint32_t plane)
{
    NS_ABORT_MSG_IF(m_sourceRouting &&
                        (plane >= m_planePaths.size() || m_planePaths[plane].empty()),
                    "QP " << m_qp.id << ": SourceRouting needs AddPath() for plane " << plane
                            << " before AddPlane()");
    m_reps.resize(plane + 1);
    RepsState& reps = m_reps[plane];
    reps.evs.assign(m_repsBufferSize, 0);
    reps.valid.assign(m_repsBufferSize, false);
}

uint32_t
RdmaSender::RepsRandomEv(uint32_t plane)
{
    s_stats.repsRandomEvs++;
    if (m_sourceRouting)
    {
        const std::vector<uint32_t>& paths = m_planePaths[plane];
        return paths[m_evRng() % paths.size()];
    }
    return m_evRng() & FLOW_LABEL_MASK;
}

int32_t
RdmaSender::RepsNextEv(uint32_t plane)
{
    RepsState& reps = m_reps[plane];
    uint32_t size = reps.evs.size();
    if (reps.explorePending)
    {
        // New or idle connection: explore for about one BDP of this plane
        reps.explorePending = false;
        uint32_t bdpPackets = (m_cc[plane].bdp + m_mtu - 1) / m_mtu;
        reps.exploreCounter = m_repsExplorePackets ? m_repsExplorePackets
                                                   : std::max<uint32_t>(1, bdpPackets);
    }

    // Exploring: every size-th packet takes a random EV
    if (reps.exploreCounter > 0)
    {
        reps.exploreCounter--;
        if (reps.exploreCounter % size == 0)
        {
            s_stats.repsExploreEvs++;
            return RepsRandomEv(plane);
        }
    }
    if (reps.numValid == 0)
    {
        return RepsRandomEv(plane);
    }

    // The oldest valid EV, from the head (the oldest slot) on
    s_stats.repsRecycledEvs++;
    for (uint32_t i = 0; i < size; i++)
    {
        uint32_t slot = (reps.head + i) % size;
        if (reps.valid[slot])
        {
            reps.valid[slot] = false;
            reps.numValid--;
            return reps.evs[slot];
        }
    }
    NS_ABORT_MSG("REPS: numValid out of sync");
    return -1;
}

void
RdmaSender::RepsOnAck(uint32_t plane, uint32_t ev, bool ecn)
{
    if (plane >= m_reps.size() || ecn)
    {
        return; // ECN-marked: the EV is not worth recycling
    }
    RepsState& reps = m_reps[plane];
    uint32_t size = reps.evs.size();
    if (!reps.valid[reps.head])
    {
        reps.numValid++;
    }
    else
    {
        s_stats.repsOverwritten++;
    }
    reps.evs[reps.head] = ev;
    reps.valid[reps.head] = true;
    reps.head = (reps.head + 1) % size;
}

void
RdmaSender::UpdateLimits(PlaneCc& cc)
{
    cc.targetRtt = m_targetRttFactor * cc.baseRtt;
    cc.bdp = std::max<uint64_t>(m_mtu, cc.rate.GetBitRate() / 8e9 * cc.baseRtt);
    cc.maxCwnd = std::max<uint64_t>(m_mtu, m_maxCwndBdp * cc.bdp);
    ClampCwnd(cc);
}

void
RdmaSender::ClampCwnd(PlaneCc& cc)
{
    cc.cwnd = std::clamp<uint32_t>(cc.cwnd, m_mtu, std::max(m_mtu, cc.maxCwnd));
}

bool
RdmaSender::CanSendNew() const
{
    return m_nextPsn < m_postedPsn && m_nextPsn - m_cumAck < m_mpr;
}

bool
RdmaSender::IsWindowOpen(uint32_t plane) const
{
    const PlaneCc& cc = m_cc[plane];
    return cc.inflightBytes == 0 || cc.inflightBytes + m_mtu <= cc.cwnd;
}

bool
RdmaSender::HasDataToSend() const
{
    if (m_lost.empty() && !CanSendNew())
    {
        return false;
    }
    for (uint32_t plane = 0; plane < m_cc.size(); plane++)
    {
        if (IsWindowOpen(plane))
        {
            return true;
        }
    }
    return false;
}

bool
RdmaSender::HasPendingData(uint32_t plane) const
{
    return (!m_lost.empty() || CanSendNew()) && IsWindowOpen(plane) &&
           (!m_sourceRouting || m_loadBalancing == LB_REPS ||
            (plane < m_activeEvs.size() && !m_activeEvs[plane].empty()));
}

int64_t
RdmaSender::GetNextSendTime(uint32_t plane) const
{
    if (!HasPendingData(plane))
    {
        return -1; // blocked: the sender wakes the planes up when this changes
    }
    const PlaneCc& cc = m_cc[plane];
    if (cc.lastSendTime < 0)
    {
        return 0;
    }
    return cc.lastSendTime + (uint64_t(m_mtu) * 8 * 1000000000ULL) / cc.rate.GetBitRate();
}

Ptr<Packet>
RdmaSender::GenerateNextPacket(uint32_t plane)
{
    NS_ASSERT_MSG(plane < m_planeWakeups.size(), "Unknown plane " << plane);
    int64_t now = Simulator::Now().GetNanoSeconds();
    if (!HasPendingData(plane) || now < GetNextSendTime(plane))
    {
        return nullptr;
    }
    int32_t ev = m_loadBalancing == LB_REPS ? RepsNextEv(plane)
                 : m_sourceRouting             ? SelectEv(plane)
                                               : -1;

    uint32_t psn;
    bool retransmission = !m_lost.empty();
    if (retransmission)
    {
        psn = *m_lost.begin(); // earliest hole first
        m_lost.erase(m_lost.begin());
    }
    else
    {
        psn = m_nextPsn++;
        GrowWindow();
    }

    const Message& msg = MessageOf(psn);
    uint64_t offset = uint64_t(psn - msg.firstPsn) << m_mtuShift;
    PacketInfo& info = Info(psn);
    info.state = IN_FLIGHT;
    info.retransmitted = retransmission;
    info.payload = std::min<uint64_t>(m_mtu, msg.size > offset ? msg.size - offset : 0);
    info.sendTime = now;
    info.ev = ev;
    info.plane = plane;
    m_cc[plane].inflightBytes += info.payload;
    m_cc[plane].lastSendTime = now;

    if (!m_rtoEvent.IsPending())
    {
        RestartRto();
    }
    return BuildPacket(msg, psn, retransmission, ev);
}

uint32_t
RdmaSender::NextEntropy()
{
    uint64_t key[2] = {m_qp.id, m_packetCount};
    return Hash32(reinterpret_cast<const char*>(key), sizeof(key)) & FLOW_LABEL_MASK;
}

Ptr<Packet>
RdmaSender::BuildPacket(const Message& msg, uint32_t psn, bool retransmission, int32_t ev)
{
    uint32_t payload = Info(psn).payload;
    uint32_t index = psn - msg.firstPsn;
    bool first = (index == 0);
    bool last = (index == msg.numPackets - 1);

    BthHeader bth;
    if (first && last)
        bth.SetOpcode(BthHeader::MRC_WRITE_ONLY_WITH_IMM);
    else if (first)
        bth.SetOpcode(BthHeader::MRC_WRITE_FIRST);
    else if (last)
        bth.SetOpcode(BthHeader::MRC_WRITE_LAST_WITH_IMM);
    else
        bth.SetOpcode(BthHeader::MRC_WRITE_MIDDLE);
    bth.SetDestQp(m_qp.id);
    bth.SetPsn(psn); // 24 bits on the wire
    bth.SetPadCnt((4 - payload % 4) % 4);
    bth.SetAckReq();
    bth.SetRTX(retransmission);

    MrcMeth meth;
    meth.msn = msg.msn;
    meth.numPackets = std::min<uint32_t>(msg.numPackets, UINT16_MAX);
    bth.SetMeth(meth);
    if (m_useTimestamp)
    {
        MrcTseth tseth;
        tseth.timestamp = Simulator::Now().GetNanoSeconds();
        bth.SetTseth(tseth);
    }
    MrcReth reth;
    reth.virtualAddress = uint64_t(index) << m_mtuShift; // offset in the message buffer
    reth.rkey = m_qp.id;
    reth.dmaLength = payload;
    bth.SetReth(reth);
    if (last)
    {
        bth.SetImmData(msg.id); // the responder completes the message with it
    }

    Ptr<Packet> packet = Create<Packet>(payload);
    packet->AddHeader(bth);

    UdpHeader udpHeader;
    udpHeader.SetSourcePort(0xC000 | (m_qp.id & 0x3FFF));
    udpHeader.SetDestinationPort(BthHeader::ROCE_UDP_PORT);
    packet->AddHeader(udpHeader);

    uint8_t nextHeader = IPV6_NEXT_HEADER_UDP;
    if (m_sourceRouting)
    {
        // Source routed: the EV names the path; the switches follow the SRH
        NS_ASSERT_MSG(ev >= 0, "Source-routed packet without an EV");
        Srv6RoutingHeader srh;
        srh.SetNextHeader(IPV6_NEXT_HEADER_UDP);
        srh.SetPath({m_evs[ev].path});
        packet->AddHeader(srh);
        nextHeader = Srv6RoutingHeader::IPV6_NEXT_HEADER_ROUTING;
        m_entropy = ev;
    }
    else if (ev >= 0)
    {
        m_entropy = ev; // REPS: the flow label to hash on
    }
    else if (m_packetSpraying)
    {
        m_entropy = m_ecmpEvs.empty() ? NextEntropy()
                                      : m_ecmpEvs[m_packetCount % m_ecmpEvs.size()];
    }
    m_packetCount++;
    s_stats.packets++;
    s_stats.retransmissions += retransmission;

    Ipv6Header ipv6Header;
    ipv6Header.SetSource(m_srcAddress);
    ipv6Header.SetDestination(m_dstAddress);
    ipv6Header.SetNextHeader(nextHeader);
    ipv6Header.SetHopLimit(64);
    ipv6Header.SetDscp(Ipv6Header::DscpDefault); // data: low priority
    ipv6Header.SetEcn(Ipv6Header::EcnType(0)); // no marks yet (see PointToPointQueue::ECN_MARK_*)
    ipv6Header.SetFlowLabel(m_entropy);
    ipv6Header.SetPayloadLength(packet->GetSize());
    packet->AddHeader(ipv6Header);
    return packet;
}

void
RdmaSender::Receive(Ptr<Packet> packet, uint32_t plane)
{
    Ipv6Header ipv6Header;
    BthHeader bth;
    if (!RdmaReceiver::ParseHeaders(packet, ipv6Header, bth) || bth.GetDestQp() != m_qp.id)
    {
        return;
    }

    switch (bth.GetOpcode())
    {
    case BthHeader::MRC_SACK:
        ProcessSack(bth, plane);
        break;
    case BthHeader::MRC_NACK:
        ProcessNack(bth);
        break;
    default:
        NS_LOG_WARN("QP " << m_qp.id << ": unexpected opcode 0x" << std::hex
                            << +bth.GetOpcode());
        return;
    }

    if (HasDataToSend())
    {
        NotifyPlanes();
    }
}

uint32_t
RdmaSender::Acknowledge(uint32_t psn)
{
    PacketInfo& info = Info(psn);
    switch (info.state)
    {
    case IN_FLIGHT:
        m_cc[info.plane].inflightBytes -= info.payload;
        break;
    case LOST:
        // Acknowledged before its retransmission: the loss was spurious reordering
        m_lost.erase(psn);
        WidenReorderWindow();
        break;
    default:
        return 0; // already acknowledged
    }
    info.state = SACKED;
    m_rackXmitTime = std::max(m_rackXmitTime, info.sendTime);
    return info.payload;
}

void
RdmaSender::ProcessSack(const BthHeader& bth, uint32_t plane)
{
    const MrcSeth& seth = bth.GetSeth();
    const MrcCcState& cc = bth.GetCcState();
    int64_t now = Simulator::Now().GetNanoSeconds();

    // RTT sample of the packet that triggered this SACK, taken before its state is cleared
    int64_t rtt = -1;
    uint32_t psn = BthHeader::UnwrapPsn(bth.GetPsn(), m_cumAck);
    if (m_useTimestamp && cc.echoTimestamp != 0)
    {
        rtt = now - int64_t(cc.echoTimestamp) - cc.serviceTime;
    }
    else if (psn >= m_cumAck && psn < m_nextPsn && Info(psn).state != EMPTY &&
             !Info(psn).retransmitted)
    {
        rtt = now - Info(psn).sendTime - cc.serviceTime;
    }
    // Congestion control of the plane the SACK came back on (the plane of the
    // packet that triggered it): every packet's own SACK counts its bytes once,
    // even when an earlier SACK's bitmap already acknowledged it
    uint32_t ccBytes = m_mtu;
    if (psn >= m_cumAck && psn < m_nextPsn && Info(psn).state != EMPTY)
    {
        ccBytes = Info(psn).payload;
    }
    if (seth.flags & MrcSeth::FLAG_DUPLICATE)
    {
        WidenReorderWindow(); // the receiver already had it: a spurious retransmission
        ccBytes = 0;
    }
    bool ecn = bth.GetCNP() || cc.ecnMarkedPackets > 0;
    if (m_sourceRouting && m_evOnEcn && cc.ecnMarkedPackets > 0)
    {
        OnEvSignal(seth.reflectedEv); // ECN mark: this path is congested
    }
    if (m_loadBalancing == LB_REPS)
    {
        // The plane the reflected EV was sent on: the path's plane, or (ECMP)
        // the plane of the packet if it carried that EV
        uint32_t ev = seth.reflectedEv;
        if (m_sourceRouting && ev < m_evs.size())
        {
            RepsOnAck(m_evs[ev].plane, ev, cc.ecnMarkedPackets > 0);
        }
        else if (!m_sourceRouting && psn >= m_cumAck && psn < m_nextPsn &&
                 Info(psn).state != EMPTY && Info(psn).ev == int32_t(ev))
        {
            RepsOnAck(Info(psn).plane, ev, cc.ecnMarkedPackets > 0);
        }
    }

    uint32_t ackedBytes = 0;

    // Cumulative ack
    uint32_t newCumAck = std::min(seth.cumulativeAck, m_nextPsn);
    bool advanced = m_cumAck < newCumAck;
    while (m_cumAck < newCumAck)
    {
        ackedBytes += Acknowledge(m_cumAck);
        Info(m_cumAck) = PacketInfo();
        m_cumAck++;
    }

    // Selective acks: the bitmap, plus the PSN that triggered this SACK
    // (it may lie beyond the 64-bit bitmap)
    uint32_t windowEnd = std::min(m_nextPsn, m_cumAck + m_mpr);
    for (uint32_t i = 0; i <= 64; i++)
    {
        uint32_t sacked;
        if (i < 64)
        {
            if (!(seth.sackBitmap & (uint64_t(1) << i)))
            {
                continue;
            }
            sacked = seth.sackBase + i;
        }
        else
        {
            sacked = psn;
        }
        if (sacked < m_cumAck || sacked >= windowEnd)
        {
            continue;
        }
        ackedBytes += Acknowledge(sacked);
    }
    if (ackedBytes > 0)
    {
        DetectReorderLoss();
        m_rto = m_minRto;
        RestartRto(); // stops the timer once nothing is outstanding
    }
    if (ccBytes > 0 && plane < m_cc.size())
    {
        OnAck(m_cc[plane], ccBytes, ecn, rtt);
    }
    if (advanced)
    {
        CompleteMessages();
    }
}

void
RdmaSender::CompleteMessages()
{
    while (!m_messages.empty() &&
           m_messages.front().firstPsn + m_messages.front().numPackets <= m_cumAck)
    {
        uint32_t id = m_messages.front().id;
        m_messages.pop_front();
        s_stats.messages++;
        if (!m_onMessageAcked.IsNull())
        {
            m_onMessageAcked(id); // may post the next message
        }
    }
}

void
RdmaSender::ProcessNack(const BthHeader& bth)
{
    const MrcNeth& neth = bth.GetNeth();
    NS_LOG_INFO("QP " << m_qp.id << ": NACK code " << +neth.code << " for PSN " << neth.psn);
    if (neth.psn < m_cumAck || neth.psn >= m_nextPsn || Info(neth.psn).state != IN_FLIGHT)
    {
        return;
    }
    if (m_sourceRouting && m_evOnLoss && neth.code == MrcNeth::NACK_TRIMMED)
    {
        OnEvSignal(Info(neth.psn).ev);
    }
    PlaneCc& cc = m_cc[Info(neth.psn).plane];
    MarkLost(neth.psn);

    // A trim is a congestion signal for the plane it was sent on: shrink its
    // window and adapt on the next period
    s_stats.trimNacks += neth.code == MrcNeth::NACK_TRIMMED;
    if (neth.code == MrcNeth::NACK_TRIMMED && cc.bytesIgnored >= cc.bytesToIgnore)
    {
        uint32_t payload = Info(neth.psn).payload;
        cc.cwnd = cc.cwnd > payload ? cc.cwnd - payload : 0;
        ClampCwnd(cc);
        cc.qaTriggered = true;
    }
}

void
RdmaSender::MarkLost(uint32_t psn)
{
    PacketInfo& info = Info(psn);
    if (info.state != IN_FLIGHT)
    {
        return;
    }
    info.state = LOST;
    m_cc[info.plane].inflightBytes -= info.payload;
    m_lost.insert(psn);
}

void
RdmaSender::WidenReorderWindow()
{
    m_reorderWindowRtts = std::min(MAX_REORDER_WINDOW_RTTS, std::max(m_reorderWindowRtts * 2, 1.0));
    s_stats.spuriousDetections++;
}

void
RdmaSender::DetectReorderLoss()
{
    int64_t now = Simulator::Now().GetNanoSeconds();
    for (uint32_t psn = m_cumAck; psn < m_nextPsn; psn++)
    {
        PacketInfo& info = Info(psn);
        if (info.state != IN_FLIGHT)
        {
            continue;
        }
        // RTT of the plane it was sent on
        const PlaneCc& cc = m_cc[info.plane];
        double rtt = cc.avgRtt > 0 ? cc.avgRtt : cc.baseRtt;
        double deadline = rtt + m_reorderWindowRtts * cc.baseRtt;
        if (info.sendTime >= m_rackXmitTime)
        {
            if (!info.retransmitted)
            {
                break; // new packets are sent in PSN order: none later is older
            }
            continue;
        }
        // A packet sent after this one was delivered: lost once it is too old
        if (now - info.sendTime > deadline)
        {
            // No EV change: with spraying this is mostly reordering, not a failed
            // path (ECN and trims already steer away from congestion)
            s_stats.reorderLosses++;
            MarkLost(psn);
        }
    }
}

void
RdmaSender::OnAck(PlaneCc& cc, uint32_t ackedBytes, bool ecn, int64_t rtt)
{
    cc.qaAcked += ackedBytes;
    QuickAdapt(cc);
    if (cc.bytesIgnored < cc.bytesToIgnore)
    {
        // Signals of packets sent before Quick Adapt resized the window
        cc.bytesIgnored += ackedBytes;
        return;
    }

    if (rtt >= 0)
    {
        cc.avgRtt = cc.avgRtt == 0 ? rtt : cc.avgRtt + RTT_EWMA_GAIN * (rtt - cc.avgRtt);
        if (rtt < cc.minRttSample)
        {
            // Best guess of the unloaded RTT: the minimum sample, which replaces
            // the configured estimate (propagation + fixed serialization)
            cc.minRttSample = rtt;
            cc.baseRtt = rtt;
            UpdateLimits(cc);
        }
    }
    else
    {
        rtt = cc.avgRtt > 0 ? cc.avgRtt : cc.baseRtt;
    }

    double cwnd = cc.cwnd;
    double mtu = m_mtu;

    // Fast Increase: a whole window went uncongested at (about) base RTT
    if (!ecn && rtt <= cc.baseRtt + FAST_INCREASE_SLACK * (cc.targetRtt - cc.baseRtt))
    {
        cc.fastIncreaseCount += ackedBytes;
        if (cc.fastIncreaseActive || cc.fastIncreaseCount > cc.cwnd)
        {
            cc.fastIncreaseActive = true;
            cc.cwnd = std::min<double>(UINT32_MAX, cwnd + m_fastIncrease * mtu);
            ClampCwnd(cc);
            return;
        }
    }
    else
    {
        cc.fastIncreaseCount = 0;
        cc.fastIncreaseActive = false;
    }

    int64_t now = Simulator::Now().GetNanoSeconds();
    double fair = ackedBytes / cwnd * cc.bdp * m_fairIncrease;
    if (ecn && rtt > cc.targetRtt)
    {
        // Congested: multiplicative decrease, at most once per base RTT
        if (cc.lastDecreaseTime < 0 || now - cc.lastDecreaseTime >= cc.baseRtt)
        {
            double avg = std::max<double>(cc.avgRtt, rtt);
            double factor = std::max(MD_MIN_FACTOR, 1 - (avg - cc.targetRtt) / avg * MD_GAIN);
            cwnd *= std::min(1.0, factor);
            cc.lastDecreaseTime = now;
        }
    }
    else if (ecn)
    {
        // Congestion building up at a switch: leave it to load balancing
    }
    else if (rtt > cc.targetRtt)
    {
        cwnd += fair; // congestion going down
    }
    else
    {
        // Underloaded: proportional increase towards the target RTT, at most
        // the acknowledged bytes (doubling per RTT), plus fair increase
        double pi = double(cc.baseRtt) / (cc.targetRtt - cc.baseRtt);
        double proportional = (cc.targetRtt - rtt) / double(std::max<int64_t>(rtt, 1)) * mtu * pi;
        cwnd += std::min<double>(ackedBytes, proportional) + fair;
    }
    cc.cwnd = std::min<double>(UINT32_MAX, cwnd);
    ClampCwnd(cc);
}

void
RdmaSender::QuickAdapt(PlaneCc& cc)
{
    int64_t now = Simulator::Now().GetNanoSeconds();
    if (now < cc.qaEnd)
    {
        return;
    }
    if (cc.qaTriggered && cc.qaEnd != 0)
    {
        // Rate the network actually delivered over the last target RTT
        cc.qaTriggered = false;
        cc.cwnd = std::min<uint64_t>(UINT32_MAX, cc.qaAcked);
        ClampCwnd(cc);
        cc.bytesToIgnore = cc.inflightBytes;
        cc.bytesIgnored = 0;
        cc.fastIncreaseCount = 0;
        cc.fastIncreaseActive = false;
    }
    cc.qaEnd = now + cc.targetRtt;
    cc.qaAcked = 0;
}

void
RdmaSender::RestartRto()
{
    Simulator::Cancel(m_rtoEvent);
    if (m_cumAck < m_nextPsn)
    {
        m_rtoEvent = Simulator::Schedule(m_rto, &RdmaSender::OnRto, this);
    }
}

void
RdmaSender::OnRto()
{
    if (m_sourceRouting && m_evOnLoss && Info(m_cumAck).state == IN_FLIGHT)
    {
        OnEvSignal(Info(m_cumAck).ev); // the oldest hole timed out on its path
    }
    bool anyLost = false;
    for (uint32_t psn = m_cumAck; psn < m_nextPsn; psn++)
    {
        if (Info(psn).state == IN_FLIGHT)
        {
            m_cc[Info(psn).plane].qaTriggered = true; // Quick Adapt on every plane that lost packets
            MarkLost(psn);
            anyLost = true;
        }
    }
    if (!anyLost)
    {
        return;
    }

    s_stats.rtos++;
    NS_LOG_INFO("QP " << m_qp.id << ": RTO " << m_rto.As(Time::US) << ", retransmitting "
                        << m_lost.size() << " packets from PSN " << *m_lost.begin());
    m_rto = std::min(m_rto * 2, m_maxRto);
    if (!m_packetSpraying)
    {
        m_entropy = NextEntropy(); // move the flow to another path
    }
    NotifyPlanes();
}

void
RdmaSender::NotifyPlanes()
{
    for (auto& wakeup : m_planeWakeups)
    {
        if (!wakeup.IsNull())
        {
            wakeup();
        }
    }
}

} // namespace ns3
