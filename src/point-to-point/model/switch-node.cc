#include "switch-node.h"

#include "ns3/hash.h"
#include "ns3/ipv6-header.h"
#include "ns3/log.h"
#include "ns3/net-device.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/udp-header.h"

#include "bth-header.h"
#include "packet-tag.h"
#include "pfc-header.h"
#include "point-to-point-channel.h"
#include "point-to-point-net-device.h"
#include "point-to-point-queue.h"
#include "ppp-header.h"
#include "srv6-header.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("SwitchNode");

NS_OBJECT_ENSURE_REGISTERED(SwitchNode);

TypeId
SwitchNode::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::SwitchNode")
            .SetParent<Node>()
            .SetGroupName("PointToPoint")
            .AddConstructor<SwitchNode>();
    return tid;
}

SwitchNode::SwitchNode()
    : Node()
{
    // m_ecnRand is not built by CreateObject, so its attributes are never
    // applied: set the antithetic flag, left uninitialized otherwise
    m_ecnRand.SetAntithetic(false);
}

SwitchNode::~SwitchNode()
{
}

uint32_t
SwitchNode::AddDevice(Ptr<NetDevice> device)
{
    NS_LOG_FUNCTION(this << device);

    uint32_t index = GetNDevices();
    m_devices.push_back(device);
    device->SetNode(this);
    device->SetIfIndex(index);
    device->SetReceiveCallback(MakeCallback(&SwitchNode::ReceiveFromDevice, this));
    Simulator::ScheduleWithContext(GetId(), Seconds(0.0), &NetDevice::Initialize, device);
    NotifyDeviceAdded(device);

    Ptr<PointToPointNetDevice> ptpDev = DynamicCast<PointToPointNetDevice>(device);
    if (ptpDev)
    {
        Ptr<PointToPointChannel> channel = DynamicCast<PointToPointChannel>(ptpDev->GetChannel());
        double bitRate = ptpDev->GetDataRate().GetBitRate();

        // Headroom: one RTT of line-rate bytes plus a few max-size packets in flight
        int32_t headroom = bitRate * channel->GetDelay().GetSeconds() * 2.0 / 8.0 +
                           HEADROOM_EXTRA_PACKETS * MAX_PACKET_SIZE;
        int32_t portBuffer = bitRate / 1e9 * BUFFER_BYTES_PER_GBPS;

        PortState& port = m_ports[ptpDev];
        port.headroomSize = headroom;
        port.ecnKmin = ECN_KMIN_RATIO * portBuffer;
        port.ecnKmax = ECN_KMAX_RATIO * portBuffer;

        m_sharedPoolSize += portBuffer - PORT_RESERVED_BYTES - headroom;
        if (portBuffer - PORT_RESERVED_BYTES - headroom < 0)
        {
            NS_LOG_WARN("Port " << index << " has less buffer (" << portBuffer
                                << "B) than reserved + headroom ("
                                << PORT_RESERVED_BYTES + headroom << "B)");
        }
    }
    return index;
}

bool
SwitchNode::ReceiveFromDevice(Ptr<NetDevice> device,
                              Ptr<const Packet> packet,
                              uint16_t protocol,
                              const Address& from)
{
    return IngressPipeline(packet->Copy(), protocol, DynamicCast<PointToPointNetDevice>(device));
}

void
SwitchNode::AddRoute(Ipv6Address dst, uint32_t ifIndex)
{
    m_routes[dst].push_back(ifIndex);
}

void
SwitchNode::AddSidRoute(uint32_t locator, uint16_t usid, uint32_t ifIndex)
{
    m_sidRoutes[SidKey(locator, usid)].push_back(ifIndex);
}

void
SwitchNode::SetEcmpSeed(uint32_t seed)
{
    m_ecmpSeed = seed;
}

uint64_t
SwitchNode::GetTxBytes(uint32_t ifIndex) const
{
    Ptr<PointToPointNetDevice> dev = DynamicCast<PointToPointNetDevice>(GetDevice(ifIndex));
    auto it = m_ports.find(dev);
    return it == m_ports.end() ? 0 : it->second.txBytes;
}

void
SwitchNode::SetEcnEnabled(uint32_t ifIndex, bool enabled)
{
    Ptr<PointToPointNetDevice> dev = DynamicCast<PointToPointNetDevice>(GetDevice(ifIndex));
    NS_ABORT_MSG_IF(dev == nullptr, "SetEcnEnabled: device " << ifIndex << " is not point-to-point");
    m_ports.at(dev).ecnEnabled = enabled;
}

void
SwitchNode::SetEcnAtDequeue(bool atDequeue)
{
    m_ecnAtDequeue = atDequeue;
}

void
SwitchNode::SetPfcEnabled(bool enabled)
{
    m_pfcEnabled = enabled;
}

void
SwitchNode::SetTrimEnabled(bool enabled)
{
    m_trimEnabled = enabled;
}

void
SwitchNode::SetSwitchId(uint32_t plane, uint16_t switchId)
{
    m_plane = plane;
    m_switchId = switchId;
    m_ecnRand.SetStream(SidKey(plane, switchId));
}

uint32_t
SwitchNode::GetPlane() const
{
    return m_plane;
}

uint16_t
SwitchNode::GetSwitchId() const
{
    return m_switchId;
}

bool
SwitchNode::IngressPipeline(Ptr<Packet> packet, uint16_t protocol, Ptr<PointToPointNetDevice> inPort)
{
    NS_ABORT_MSG_IF(protocol == PROTOCOL_IPV4,
                    "Switch " << m_plane << ":" << m_switchId
                              << " received an IPv4 packet; only IPv6 is supported");
    if (protocol != PROTOCOL_IPV6)
    {
        NS_LOG_WARN("Drop non-IPv6 packet (protocol 0x" << std::hex << protocol << std::dec << ")");
        return false;
    }

    int32_t size = packet->GetSize();
    PortState& in = m_ports.at(inPort);

    Ipv6Header ipv6Header;
    packet->RemoveHeader(ipv6Header);
    bool lowPriority =
        PointToPointQueue::GetPriority(ipv6Header) == PointToPointQueue::PRIORITY_LOW;

    // Admission (low priority only): must fit in reserved, shared (dynamic threshold) or headroom
    if (lowPriority)
    {
        bool fitsReserved = in.ingressUsed + size <= PORT_RESERVED_BYTES;
        bool fitsShared = GetPortSharedUsed(in) + size <= GetSharedThreshold();
        bool fitsHeadroom = in.headroomUsed + size <= in.headroomSize;
        bool admitted = fitsReserved || fitsShared || fitsHeadroom;

        Ptr<Packet> trimmed = (!admitted && m_trimEnabled) ? TrimPacket(packet, ipv6Header) : nullptr;
        if (trimmed != nullptr)
        {
            // Forward the header-only packet as high priority (no MMU accounting)
            m_trimCount += 1;
            packet = trimmed;
            lowPriority = false;
        }
        else if (!admitted)
        {
            m_dropCount += 1;
            if (m_pfcEnabled)
            {
                NS_LOG_WARN("Drop under PFC (packet " << size << "B, headroom "
                                                      << in.headroomUsed << "/" << in.headroomSize
                                                      << "B)");
            }
            return false;
        }
    }

    // Routing (extension headers and payload are left untouched)
    uint8_t hopLimit = ipv6Header.GetHopLimit();
    if (hopLimit <= 1)
    {
        NS_LOG_WARN("Drop packet with expired hop limit");
        return false;
    }
    ipv6Header.SetHopLimit(hopLimit - 1);

    const std::vector<uint32_t>* route = LookupNextHops(packet, ipv6Header);
    if (route == nullptr)
    {
        return false;
    }
    const std::vector<uint32_t>& nextHops = *route;

    uint32_t hash = nextHops.size() > 1 ? EcmpHash(ipv6Header) : 0;
    uint32_t outIfIndex = nextHops[hash % nextHops.size()];
    NS_ABORT_MSG_IF(outIfIndex >= GetNDevices(), "Invalid next-hop device " << outIfIndex);
    Ptr<PointToPointNetDevice> outPort =
        DynamicCast<PointToPointNetDevice>(GetDevice(outIfIndex));
    NS_ABORT_MSG_IF(outPort == nullptr,
                    "Next-hop device " << outIfIndex << " is not a PointToPointNetDevice");
    if (!outPort->IsLinkUp())
    {
        // Drop before accounting: a failed Send would never release the buffer
        m_dropCount += 1;
        return false;
    }

    if (!lowPriority)
    {
        packet->AddHeader(ipv6Header);
        if (!outPort->Send(packet, outPort->GetBroadcast(), protocol))
        {
            NS_LOG_WARN("High-priority queue overflow on device " << outIfIndex);
            return false;
        }
        return true;
    }

    // Buffer accounting (must precede Send: the egress pipeline may run inside it)
    PortState& out = m_ports.at(outPort);
    out.egressUsed += size;

    int32_t newIngress = in.ingressUsed + size;
    if (newIngress <= PORT_RESERVED_BYTES)
    {
        in.ingressUsed = newIngress;
    }
    else if (newIngress - PORT_RESERVED_BYTES > GetSharedThreshold())
    {
        in.headroomUsed += size;
        if (m_pfcEnabled && !in.paused && in.headroomUsed > MAX_PACKET_SIZE)
        {
            NS_LOG_WARN("Headroom " << in.headroomUsed << "B used without PFC pause");
        }
    }
    else
    {
        m_sharedPoolUsed += std::min(size, newIngress - PORT_RESERVED_BYTES);
        in.ingressUsed = newIngress;
    }

    PacketTag packetTag;
    packetTag.SetSize(size);
    packetTag.SetNetDevice(inPort);
    packet->ReplacePacketTag(packetTag);

    if (ShouldPause(in))
    {
        SendPfc(inPort, true);
    }

    // ECN marking at enqueue, on egress occupancy including this packet
    if (!m_ecnAtDequeue && ipv6Header.GetEcn() != Ipv6Header::ECN_NotECT && ShouldMarkEcn(out))
    {
        m_ecnMarkCount += 1;
        ipv6Header.SetEcn(Ipv6Header::ECN_CE);
    }

    packet->AddHeader(ipv6Header);

    if (!outPort->Send(packet, outPort->GetBroadcast(), protocol))
    {
        NS_LOG_WARN("Egress queue overflow on device " << outIfIndex);
        return false;
    }
    return true;
}

Ptr<Packet>
SwitchNode::EgressPipeline(Ptr<Packet> packet, uint16_t protocol, Ptr<PointToPointNetDevice> outPort)
{
    // Only low-priority packets hold MMU buffer
    if (protocol != PROTOCOL_IPV6 ||
        PointToPointQueue::GetPriority(packet) != PointToPointQueue::PRIORITY_LOW)
    {
        return packet;
    }

    PacketTag packetTag;
    bool tagged = packet->PeekPacketTag(packetTag);
    NS_ASSERT_MSG(tagged, "Low-priority egress packet without PacketTag");
    if (!tagged)
    {
        return packet;
    }

    Ptr<PointToPointNetDevice> inPort = packetTag.GetNetDevice();
    PortState& in = m_ports.at(inPort);
    PortState& out = m_ports.at(outPort);
    int32_t size = packetTag.GetSize();

    out.egressUsed -= size;
    out.txBytes += size;

    // Release headroom first, then shared, then reserved
    int32_t fromHeadroom = std::min(size, in.headroomUsed);
    in.headroomUsed -= fromHeadroom;

    int32_t remain = size - fromHeadroom;
    m_sharedPoolUsed -= std::min(remain, GetPortSharedUsed(in));
    in.ingressUsed -= remain;

    NS_ASSERT_MSG(out.egressUsed >= 0 && in.headroomUsed >= 0 && m_sharedPoolUsed >= 0 &&
                      in.ingressUsed >= 0,
                  "Negative buffer counter (egress " << out.egressUsed << ", headroom "
                                                     << in.headroomUsed << ", shared "
                                                     << m_sharedPoolUsed << ", ingress "
                                                     << in.ingressUsed << ")");

    // ECN marking at dequeue, on the egress queue this packet leaves behind
    if (m_ecnAtDequeue && ShouldMarkEcn(out))
    {
        PppHeader ppp;
        Ipv6Header ipv6Header;
        packet->RemoveHeader(ppp);
        packet->RemoveHeader(ipv6Header);
        if (ipv6Header.GetEcn() != Ipv6Header::ECN_NotECT)
        {
            m_ecnMarkCount += 1;
            ipv6Header.SetEcn(Ipv6Header::ECN_CE);
        }
        packet->AddHeader(ipv6Header);
        packet->AddHeader(ppp);
    }

    if (ShouldResume(in))
    {
        SendPfc(inPort, false);
    }
    return packet;
}

int32_t
SwitchNode::GetPortSharedUsed(const PortState& port)
{
    return std::max(0, port.ingressUsed - PORT_RESERVED_BYTES);
}

int32_t
SwitchNode::GetSharedThreshold() const
{
    return m_sharedPoolSize - m_sharedPoolUsed;
}

bool
SwitchNode::ShouldMarkEcn(const PortState& port)
{
    if (!port.ecnEnabled || port.egressUsed < port.ecnKmin)
    {
        return false;
    }
    if (port.egressUsed > port.ecnKmax)
    {
        return true;
    }
    double prob =
        ECN_PMAX * (port.egressUsed - port.ecnKmin) / (port.ecnKmax - port.ecnKmin);
    return m_ecnRand.GetValue(0.0, 1.0) < prob;
}

bool
SwitchNode::ShouldPause(const PortState& port) const
{
    if (!m_pfcEnabled || port.paused)
    {
        return false;
    }
    return port.headroomUsed > 0 || GetPortSharedUsed(port) >= GetSharedThreshold();
}

bool
SwitchNode::ShouldResume(const PortState& port) const
{
    if (!port.paused)
    {
        return false;
    }
    int32_t sharedUsed = GetPortSharedUsed(port);
    return port.headroomUsed == 0 &&
           (sharedUsed == 0 || sharedUsed + PFC_RESUME_OFFSET <= GetSharedThreshold());
}

void
SwitchNode::SendPfc(Ptr<PointToPointNetDevice> port, bool pause)
{
    m_ports.at(port).paused = pause;

    Ptr<Packet> packet = Create<Packet>();
    packet->AddHeader(PfcHeader(PointToPointQueue::PRIORITY_LOW, pause));

    if (!port->Send(packet, port->GetBroadcast(), PROTOCOL_PFC))
    {
        NS_LOG_WARN("Failed to send PFC "
                  << (pause ? "pause" : "resume"));
    }
}

Ptr<Packet>
SwitchNode::TrimPacket(Ptr<Packet> packet, Ipv6Header& ipv6Header) const
{
    uint8_t nextHeader = ipv6Header.GetNextHeader();
    Srv6RoutingHeader srh;
    bool hasSrh = false;
    if (nextHeader == Srv6RoutingHeader::IPV6_NEXT_HEADER_ROUTING)
    {
        uint8_t srhPrefix[3];
        if (packet->CopyData(srhPrefix, sizeof(srhPrefix)) != sizeof(srhPrefix) ||
            srhPrefix[2] != Srv6RoutingHeader::ROUTING_TYPE_SRH)
        {
            return nullptr;
        }
        packet->RemoveHeader(srh);
        hasSrh = true;
        nextHeader = srh.GetNextHeader();
    }
    if (nextHeader != Srv6RoutingHeader::IPV6_NEXT_HEADER_UDP)
    {
        return nullptr;
    }

    UdpHeader udpHeader;
    packet->RemoveHeader(udpHeader);
    if (udpHeader.GetDestinationPort() != BthHeader::ROCE_UDP_PORT)
    {
        return nullptr;
    }

    BthHeader bthHeader;
    packet->RemoveHeader(bthHeader);
    bthHeader.SetTrimmed();

    // Headers only: the payload stays in (and is dropped with) the original
    Ptr<Packet> trimmed = Create<Packet>();
    trimmed->AddHeader(bthHeader);
    trimmed->AddHeader(udpHeader); // UDP length is recomputed on serialization
    if (hasSrh)
    {
        trimmed->AddHeader(srh);
    }

    ipv6Header.SetPayloadLength(trimmed->GetSize());
    ipv6Header.SetDscp(PointToPointQueue::DSCP_HIGH_PRIORITY);
    return trimmed;
}

const std::vector<uint32_t>*
SwitchNode::LookupNextHops(Ptr<Packet> packet, const Ipv6Header& ipv6Header)
{
    // SRv6: the SRH directly follows the IPv6 header
    uint8_t srhPrefix[3];
    if (ipv6Header.GetNextHeader() == Srv6RoutingHeader::IPV6_NEXT_HEADER_ROUTING &&
        packet->CopyData(srhPrefix, sizeof(srhPrefix)) == sizeof(srhPrefix) &&
        srhPrefix[2] == Srv6RoutingHeader::ROUTING_TYPE_SRH)
    {
        Srv6RoutingHeader srh;
        packet->RemoveHeader(srh);

        bool active = !srh.GetActiveSid().IsEndOfCarrier();
        if (active && srh.Match(m_plane, m_switchId))
        {
            active = srh.Shift(); // false: this switch is the last endpoint of the path
        }
        packet->AddHeader(srh);

        if (active)
        {
            const Srv6Sid& sid = srh.GetActiveSid();
            auto sidRoute = m_sidRoutes.find(SidKey(sid.GetLocator(), sid.GetActiveUsid()));
            if (sidRoute == m_sidRoutes.end() || sidRoute->second.empty())
            {
                NS_LOG_WARN("No SID route to " << sid.GetLocator() << ":" << sid.GetActiveUsid());
                return nullptr;
            }
            return &sidRoute->second;
        }
        // Path exhausted: continue on the IPv6 destination
    }

    auto route = m_routes.find(ipv6Header.GetDestination());
    if (route == m_routes.end() || route->second.empty())
    {
        NS_LOG_WARN("No route to "
                  << ipv6Header.GetDestination());
        return nullptr;
    }
    return &route->second;
}

uint32_t
SwitchNode::EcmpHash(const Ipv6Header& header) const
{
    uint8_t buf[4 + 16 + 16 + 4 + 1];
    uint8_t* p = buf;
    std::memcpy(p, &m_ecmpSeed, 4);
    p += 4;
    header.GetSource().GetBytes(p);
    p += 16;
    header.GetDestination().GetBytes(p);
    p += 16;
    uint32_t flowLabel = header.GetFlowLabel();
    std::memcpy(p, &flowLabel, 4);
    p += 4;
    *p = header.GetNextHeader();
    return Hash32(reinterpret_cast<const char*>(buf), sizeof(buf));
}

} // namespace ns3
