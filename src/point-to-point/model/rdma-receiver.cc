#include "rdma-receiver.h"

#include "point-to-point-queue.h"
#include "srv6-header.h"

#include "ns3/log.h"
#include "ns3/udp-header.h"
#include "ns3/uinteger.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RdmaReceiver");

NS_OBJECT_ENSURE_REGISTERED(RdmaReceiver);

TypeId
RdmaReceiver::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RdmaReceiver")
                            .SetParent<Object>()
                            .SetGroupName("PointToPoint")
                            .AddAttribute("Mpr",
                                          "Maximum PSN Range: receive window in packets",
                                          UintegerValue(1024),
                                          MakeUintegerAccessor(&RdmaReceiver::m_mpr),
                                          MakeUintegerChecker<uint32_t>(1));
    return tid;
}

RdmaReceiver::RdmaReceiver(uint32_t qpn)
    : m_qpn(qpn),
      m_mpr(1024)
{
}

RdmaReceiver::~RdmaReceiver()
{
}

void
RdmaReceiver::NotifyConstructionCompleted()
{
    Object::NotifyConstructionCompleted();
    m_received.assign(m_mpr, false);
}

bool
RdmaReceiver::ParseHeaders(Ptr<Packet> packet, Ipv6Header& ipv6Header, BthHeader& bthHeader)
{
    packet->RemoveHeader(ipv6Header);
    uint8_t nextHeader = ipv6Header.GetNextHeader();

    if (nextHeader == Srv6RoutingHeader::IPV6_NEXT_HEADER_ROUTING)
    {
        uint8_t prefix[3];
        if (packet->CopyData(prefix, sizeof(prefix)) != sizeof(prefix) ||
            prefix[2] != Srv6RoutingHeader::ROUTING_TYPE_SRH)
        {
            return false;
        }
        Srv6RoutingHeader srh;
        packet->RemoveHeader(srh);
        nextHeader = srh.GetNextHeader();
    }
    if (nextHeader != Srv6RoutingHeader::IPV6_NEXT_HEADER_UDP)
    {
        return false;
    }

    UdpHeader udpHeader;
    packet->RemoveHeader(udpHeader);
    if (udpHeader.GetDestinationPort() != BthHeader::ROCE_UDP_PORT)
    {
        return false;
    }
    packet->RemoveHeader(bthHeader);
    return true;
}

bool
RdmaReceiver::PeekHeaders(Ptr<const Packet> packet, Ipv6Header& ipv6Header, BthHeader& bthHeader)
{
    return ParseHeaders(packet->Copy(), ipv6Header, bthHeader);
}

bool
RdmaReceiver::IsReceived(uint32_t psn) const
{
    if (psn < m_expectedPsn)
    {
        return true;
    }
    if (psn >= m_expectedPsn + m_mpr)
    {
        return false;
    }
    return m_received[psn % m_mpr];
}

Ptr<Packet>
RdmaReceiver::Receive(Ptr<Packet> packet)
{
    Ipv6Header ipv6Header;
    BthHeader bthHeader;
    if (!ParseHeaders(packet, ipv6Header, bthHeader) || !bthHeader.IsMrcRequest() ||
        bthHeader.GetDestQp() != m_qpn)
    {
        return nullptr;
    }

    uint32_t psn = BthHeader::UnwrapPsn(bthHeader.GetPsn(), m_expectedPsn);
    bool ce = PointToPointQueue::HasEcnMark(ipv6Header, PointToPointQueue::ECN_MARK_QUEUE);

    if (IsReceived(psn))
    {
        // Duplicate (e.g. spurious retransmission or a late trimmed copy)
        return BuildSack(ipv6Header, bthHeader, ce, !bthHeader.GetTrimmed());
    }
    if (bthHeader.GetTrimmed())
    {
        return BuildNack(ipv6Header, bthHeader, psn, MrcNeth::NACK_TRIMMED);
    }
    if (psn >= m_expectedPsn + m_mpr)
    {
        return BuildNack(ipv6Header, bthHeader, psn, MrcNeth::NACK_OUT_OF_MPR);
    }

    // New data: place it (out of order allowed) and advance the cumulative ack
    m_received[psn % m_mpr] = true;
    m_rcvdBytes += bthHeader.GetReth().dmaLength;
    if (bthHeader.IsMrcImm())
    {
        m_lastPackets[psn] = bthHeader.GetImmData();
    }
    while (m_received[m_expectedPsn % m_mpr])
    {
        m_received[m_expectedPsn % m_mpr] = false;
        m_expectedPsn++;
    }
    Ptr<Packet> sack = BuildSack(ipv6Header, bthHeader, ce);

    // Complete, in order, every message whose last packet the cumulative ack passed
    while (!m_lastPackets.empty() && m_lastPackets.begin()->first < m_expectedPsn)
    {
        uint32_t msgId = m_lastPackets.begin()->second;
        m_lastPackets.erase(m_lastPackets.begin());
        if (!m_onMessage.IsNull())
        {
            m_onMessage(msgId);
        }
    }
    return sack;
}

Ptr<Packet>
RdmaReceiver::BuildSack(const Ipv6Header& dataIp,
                        const BthHeader& dataBth,
                        bool ce,
                        bool duplicate) const
{
    MrcSeth seth;
    seth.cumulativeAck = m_expectedPsn;
    seth.sackBase = m_expectedPsn;
    for (uint32_t i = 0; i < SACK_BITMAP_BITS && i < m_mpr; i++)
    {
        if (m_received[(m_expectedPsn + i) % m_mpr])
        {
            seth.sackBitmap |= uint64_t(1) << i;
        }
    }
    seth.reflectedEv = dataIp.GetFlowLabel();
    seth.mpr = (m_mpr + MPR_UNIT - 1) / MPR_UNIT;
    if (duplicate)
    {
        seth.flags |= MrcSeth::FLAG_DUPLICATE; // tells the sender a retransmission was spurious
    }

    MrcCcState cc;
    cc.rcvdBytes = m_rcvdBytes;
    cc.echoTimestamp = dataBth.GetTSH() ? dataBth.GetTseth().timestamp : 0;
    cc.ecnMarkedPackets = ce ? 1 : 0;
    cc.lbMarkedPackets =
        PointToPointQueue::HasEcnMark(dataIp, PointToPointQueue::ECN_MARK_LB) ? 1 : 0;

    BthHeader bth;
    bth.SetOpcode(BthHeader::MRC_SACK);
    bth.SetPsn(dataBth.GetPsn());
    bth.SetSeth(seth);
    bth.SetCcState(cc);
    bth.SetCNP(ce);
    return Encapsulate(bth, dataIp);
}

Ptr<Packet>
RdmaReceiver::BuildNack(const Ipv6Header& dataIp,
                        const BthHeader& dataBth,
                        uint32_t psn,
                        uint8_t code) const
{
    MrcNeth neth;
    neth.psn = psn; // full 32-bit PSN
    neth.code = code;

    BthHeader bth;
    bth.SetOpcode(BthHeader::MRC_NACK);
    bth.SetPsn(dataBth.GetPsn());
    bth.SetNeth(neth);
    bth.SetCNP(true);
    return Encapsulate(bth, dataIp);
}

Ptr<Packet>
RdmaReceiver::Encapsulate(BthHeader& replyBth, const Ipv6Header& dataIp) const
{
    replyBth.SetDestQp(m_qpn);

    Ptr<Packet> reply = Create<Packet>();
    reply->AddHeader(replyBth);

    UdpHeader udpHeader;
    udpHeader.SetSourcePort(BthHeader::ROCE_UDP_PORT);
    udpHeader.SetDestinationPort(BthHeader::ROCE_UDP_PORT);
    reply->AddHeader(udpHeader);

    Ipv6Header ipv6Header;
    ipv6Header.SetSource(dataIp.GetDestination());
    ipv6Header.SetDestination(dataIp.GetSource());
    ipv6Header.SetNextHeader(Srv6RoutingHeader::IPV6_NEXT_HEADER_UDP);
    ipv6Header.SetHopLimit(64);
    ipv6Header.SetDscp(PointToPointQueue::DSCP_HIGH_PRIORITY);
    ipv6Header.SetEcn(Ipv6Header::EcnType(0)); // high priority: never marked
    ipv6Header.SetFlowLabel(dataIp.GetFlowLabel());
    ipv6Header.SetPayloadLength(reply->GetSize());
    reply->AddHeader(ipv6Header);
    return reply;
}

} // namespace ns3
