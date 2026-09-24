#include "rdma-sender.h"

#include "rdma-receiver.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
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
                          "this many base RTTs (doubled on each spurious detection, up to 16)",
                          DoubleValue(1.0),
                          MakeDoubleAccessor(&RdmaSender::m_reorderWindow),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("MinRto",
                          "Initial retransmission timeout; must exceed the worst queueing "
                          "delay (incl. PFC pauses) to avoid spurious timeouts",
                          TimeValue(MicroSeconds(200)),
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
            .AddAttribute("SourceRouting",
                          "MRC static SRv6 source routing with EV management: every EV maps "
                          "to a path given by AddPath(); ECN skips an EV, loss retires it",
                          BooleanValue(false),
                          MakeBooleanAccessor(&RdmaSender::m_sourceRouting),
                          MakeBooleanChecker())
            .AddAttribute("EvsPerPlane",
                          "SourceRouting: active EVs (random paths) per plane, the other paths "
                          "are backups; 0: half of the plane's paths (rounded up)",
                          UintegerValue(0),
                          MakeUintegerAccessor(&RdmaSender::m_evsPerPlane),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("EvMinActive",
                          "SourceRouting: an EV is not retired if its plane would keep fewer "
                          "active EVs than this",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RdmaSender::m_evMinActive),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("EcnSkipRtts",
                          "SourceRouting: base RTTs an EV is skipped after an ECN or trim signal",
                          DoubleValue(1.0),
                          MakeDoubleAccessor(&RdmaSender::m_ecnSkipRtts),
                          MakeDoubleChecker<double>(0.0));
    return tid;
}

RdmaSender::RdmaSender(QpInfo qp)
    : m_qp(qp),
      m_mtuShift(12),
      m_rttSerializationBytes(1000),
      m_mpr(1024),
      m_reorderWindow(1.0),
      m_minRto(MicroSeconds(200)),
      m_maxRto(MilliSeconds(2)),
      m_packetSpraying(true),
      m_useTimestamp(false),
      m_targetRttFactor(1.5),
      m_maxCwndBdp(1.5),
      m_fairIncrease(0.5),
      m_fastIncrease(2),
      m_sourceRouting(false),
      m_evsPerPlane(0),
      m_evMinActive(2),
      m_ecnSkipRtts(1.0),
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
    if (m_planeWakeups.empty())
    {
        m_baseRtt = m_qp.minRttNs + m_rttSerializationBytes * 8 * 1e9 / rate.GetBitRate();
    }
    m_planeWakeups.push_back(wakeup);
    m_lineRate = m_lineRate + rate;
    UpdateLimits();
    if (m_minRttSample == INT64_MAX)
    {
        // Start at the line rate of every plane. Planes register one after
        // another and the first may already have sent: keep resizing the
        // initial window until congestion control has an RTT sample.
        m_cwnd = m_bdp;
        ClampCwnd();
    }

    uint32_t plane = m_planeWakeups.size() - 1;
    if (m_sourceRouting)
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
    int64_t now = Simulator::Now().GetNanoSeconds();
    uint32_t n = active.size();
    uint32_t& cursor = m_evCursor[plane];
    for (uint32_t k = 0; k < n; k++)
    {
        uint32_t ev = active[(cursor + k) % n];
        if (m_evs[ev].skipUntil <= now)
        {
            cursor = (cursor + k + 1) % n;
            return ev;
        }
    }
    // Every active EV is congested: keep rotating rather than stall
    uint32_t ev = active[cursor % n];
    cursor = (cursor + 1) % n;
    return ev;
}

void
RdmaSender::OnEvCongested(int32_t ev)
{
    if (ev < 0 || uint32_t(ev) >= m_evs.size())
    {
        return;
    }
    m_evs[ev].skipUntil = Simulator::Now().GetNanoSeconds() + m_ecnSkipRtts * m_baseRtt;
    s_stats.evSkips++;
}

void
RdmaSender::RestoreEv(uint32_t psn)
{
    auto it = m_retiredFor.find(psn);
    if (it == m_retiredFor.end())
    {
        return;
    }
    uint32_t ev = it->second;
    m_retiredFor.erase(it);
    if (m_evs[ev].state != EV_ASSUMED_BAD)
    {
        return;
    }
    m_evs[ev].state = EV_GOOD;
    m_backupEvs[m_evs[ev].plane].push_back(ev);
    s_stats.evRestores++;
}

void
RdmaSender::OnEvLoss(int32_t ev, uint32_t psn)
{
    if (ev < 0 || uint32_t(ev) >= m_evs.size() || m_evs[ev].state != EV_GOOD)
    {
        return;
    }
    uint32_t plane = m_evs[ev].plane;
    std::vector<uint32_t>& active = m_activeEvs[plane];
    auto it = std::find(active.begin(), active.end(), uint32_t(ev));
    if (it == active.end() || (active.size() <= m_evMinActive && m_backupEvs[plane].empty()))
    {
        OnEvCongested(ev); // keep enough paths: only avoid it for a while
        return;
    }

    // Assume the path failed: replace it by a backup EV of the same plane
    m_evs[ev].state = EV_ASSUMED_BAD;
    m_retiredFor[psn] = ev;
    s_stats.evFailures++;
    NS_LOG_INFO("QP " << m_qp.id << ": EV " << ev << " (plane " << plane
                        << ") assumed bad");
    if (!m_backupEvs[plane].empty())
    {
        *it = m_backupEvs[plane].back();
        m_backupEvs[plane].pop_back();
    }
    else
    {
        active.erase(it);
        m_evCursor[plane] = active.empty() ? 0 : m_evCursor[plane] % active.size();
    }
}

void
RdmaSender::UpdateLimits()
{
    m_targetRtt = m_targetRttFactor * m_baseRtt;
    m_bdp = std::max<uint64_t>(m_mtu, m_lineRate.GetBitRate() / 8e9 * m_baseRtt);
    m_maxCwnd = std::max<uint64_t>(m_mtu, m_maxCwndBdp * m_bdp);
    ClampCwnd();
}

void
RdmaSender::ClampCwnd()
{
    m_cwnd = std::clamp<uint32_t>(m_cwnd, m_mtu, std::max(m_mtu, m_maxCwnd));
}

bool
RdmaSender::CanSendNew() const
{
    return m_nextPsn < m_postedPsn && m_nextPsn - m_cumAck < m_mpr;
}

bool
RdmaSender::IsWindowOpen() const
{
    return m_inflightBytes == 0 || m_inflightBytes + m_mtu <= m_cwnd;
}

bool
RdmaSender::HasDataToSend() const
{
    return !m_planeWakeups.empty() && IsWindowOpen() && (!m_lost.empty() || CanSendNew());
}

bool
RdmaSender::HasPendingData(uint32_t plane) const
{
    return HasDataToSend() &&
           (!m_sourceRouting || (plane < m_activeEvs.size() && !m_activeEvs[plane].empty()));
}

int64_t
RdmaSender::GetNextSendTime(uint32_t plane) const
{
    if (!HasPendingData(plane))
    {
        return -1; // blocked: the sender wakes the planes up when this changes
    }
    if (m_lastSendTime < 0)
    {
        return 0;
    }
    return m_lastSendTime + (uint64_t(m_mtu) * 8 * 1000000000ULL) / m_lineRate.GetBitRate();
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
    int32_t ev = m_sourceRouting ? SelectEv(plane) : -1;

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
    m_inflightBytes += info.payload;
    m_lastSendTime = now;

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
    if (ev >= 0)
    {
        // Source routed: the EV names the path; the switches follow the SRH
        Srv6RoutingHeader srh;
        srh.SetNextHeader(IPV6_NEXT_HEADER_UDP);
        srh.SetPath({m_evs[ev].path});
        packet->AddHeader(srh);
        nextHeader = Srv6RoutingHeader::IPV6_NEXT_HEADER_ROUTING;
        m_entropy = ev;
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
    ipv6Header.SetEcn(Ipv6Header::ECN_ECT0);
    ipv6Header.SetFlowLabel(m_entropy);
    ipv6Header.SetPayloadLength(packet->GetSize());
    packet->AddHeader(ipv6Header);
    return packet;
}

void
RdmaSender::Receive(Ptr<Packet> packet)
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
        ProcessSack(bth);
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
        m_inflightBytes -= info.payload;
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
RdmaSender::ProcessSack(const BthHeader& bth)
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
    if (seth.flags & MrcSeth::FLAG_DUPLICATE)
    {
        WidenReorderWindow(); // the receiver already had it: a spurious retransmission
        if (m_sourceRouting)
        {
            RestoreEv(psn); // and its first path did deliver it
        }
    }
    bool ecn = bth.GetCNP() || cc.ecnMarkedPackets > 0;
    if (m_sourceRouting && ecn)
    {
        OnEvCongested(seth.reflectedEv); // this path is more congested than others
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
        OnAck(ackedBytes, ecn, rtt);
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
    if (m_sourceRouting && neth.code == MrcNeth::NACK_TRIMMED)
    {
        OnEvCongested(Info(neth.psn).ev);
    }
    MarkLost(neth.psn);

    // A trim is a congestion signal: shrink the window and adapt on the next period
    s_stats.trimNacks += neth.code == MrcNeth::NACK_TRIMMED;
    if (neth.code == MrcNeth::NACK_TRIMMED && m_bytesIgnored >= m_bytesToIgnore)
    {
        uint32_t payload = Info(neth.psn).payload;
        m_cwnd = m_cwnd > payload ? m_cwnd - payload : 0;
        ClampCwnd();
        TriggerQuickAdapt();
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
    m_inflightBytes -= info.payload;
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
    double rtt = m_avgRtt > 0 ? m_avgRtt : m_baseRtt;
    double deadline = rtt + m_reorderWindowRtts * m_baseRtt;
    for (uint32_t psn = m_cumAck; psn < m_nextPsn; psn++)
    {
        PacketInfo& info = Info(psn);
        if (info.state != IN_FLIGHT)
        {
            continue;
        }
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
            if (m_sourceRouting)
            {
                OnEvLoss(info.ev, psn); // lost, not trimmed: assume the path failed
            }
            s_stats.reorderLosses++;
            MarkLost(psn);
        }
    }
}

void
RdmaSender::OnAck(uint32_t ackedBytes, bool ecn, int64_t rtt)
{
    m_qaAcked += ackedBytes;
    QuickAdapt();
    if (m_bytesIgnored < m_bytesToIgnore)
    {
        // Signals of packets sent before Quick Adapt resized the window
        m_bytesIgnored += ackedBytes;
        return;
    }

    if (rtt >= 0)
    {
        m_avgRtt = m_avgRtt == 0 ? rtt : m_avgRtt + RTT_EWMA_GAIN * (rtt - m_avgRtt);
        if (rtt < m_minRttSample)
        {
            // Best guess of the unloaded RTT: the minimum sample, which replaces
            // the configured estimate (propagation + fixed serialization)
            m_minRttSample = rtt;
            m_baseRtt = rtt;
            UpdateLimits();
        }
    }
    else
    {
        rtt = m_avgRtt > 0 ? m_avgRtt : m_baseRtt;
    }

    double cwnd = m_cwnd;
    double mtu = m_mtu;

    // Fast Increase: a whole window went uncongested at (about) base RTT
    if (!ecn && rtt <= m_baseRtt + FAST_INCREASE_SLACK * (m_targetRtt - m_baseRtt))
    {
        m_fastIncreaseCount += ackedBytes;
        if (m_fastIncreaseActive || m_fastIncreaseCount > m_cwnd)
        {
            m_fastIncreaseActive = true;
            m_cwnd = std::min<double>(UINT32_MAX, cwnd + m_fastIncrease * mtu);
            ClampCwnd();
            return;
        }
    }
    else
    {
        m_fastIncreaseCount = 0;
        m_fastIncreaseActive = false;
    }

    int64_t now = Simulator::Now().GetNanoSeconds();
    double fair = ackedBytes / cwnd * m_bdp * m_fairIncrease;
    if (ecn && rtt > m_targetRtt)
    {
        // Congested: multiplicative decrease, at most once per base RTT
        if (m_lastDecreaseTime < 0 || now - m_lastDecreaseTime >= m_baseRtt)
        {
            double avg = std::max<double>(m_avgRtt, rtt);
            double factor = std::max(MD_MIN_FACTOR, 1 - (avg - m_targetRtt) / avg * MD_GAIN);
            cwnd *= std::min(1.0, factor);
            m_lastDecreaseTime = now;
        }
    }
    else if (ecn)
    {
        // Congestion building up at a switch: leave it to load balancing
    }
    else if (rtt > m_targetRtt)
    {
        cwnd += fair; // congestion going down
    }
    else
    {
        // Underloaded: proportional increase towards the target RTT, at most
        // the acknowledged bytes (doubling per RTT), plus fair increase
        double pi = double(m_baseRtt) / (m_targetRtt - m_baseRtt);
        double proportional = (m_targetRtt - rtt) / double(std::max<int64_t>(rtt, 1)) * mtu * pi;
        cwnd += std::min<double>(ackedBytes, proportional) + fair;
    }
    m_cwnd = std::min<double>(UINT32_MAX, cwnd);
    ClampCwnd();
}

void
RdmaSender::TriggerQuickAdapt()
{
    m_qaTriggered = true;
}

void
RdmaSender::QuickAdapt()
{
    int64_t now = Simulator::Now().GetNanoSeconds();
    if (now < m_qaEnd)
    {
        return;
    }
    if (m_qaTriggered && m_qaEnd != 0)
    {
        // Rate the network actually delivered over the last target RTT
        m_qaTriggered = false;
        m_cwnd = std::min<uint64_t>(UINT32_MAX, m_qaAcked);
        ClampCwnd();
        m_bytesToIgnore = m_inflightBytes;
        m_bytesIgnored = 0;
        m_fastIncreaseCount = 0;
        m_fastIncreaseActive = false;
    }
    m_qaEnd = now + m_targetRtt;
    m_qaAcked = 0;
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
    if (m_sourceRouting && Info(m_cumAck).state == IN_FLIGHT)
    {
        OnEvLoss(Info(m_cumAck).ev, m_cumAck); // the oldest hole timed out on its path
    }
    bool anyLost = false;
    for (uint32_t psn = m_cumAck; psn < m_nextPsn; psn++)
    {
        if (Info(psn).state == IN_FLIGHT)
        {
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
    TriggerQuickAdapt();
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
