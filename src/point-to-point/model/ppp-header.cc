#include "ppp-header.h"

#include "ns3/abort.h"
#include "ns3/assert.h"
#include "ns3/header.h"
#include "ns3/log.h"

#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("PppHeader");

NS_OBJECT_ENSURE_REGISTERED(PppHeader);

FlowV4Id::FlowV4Id(const FlowV4Id& o)
{
    m_srcIP = o.m_srcIP;
    m_dstIP = o.m_dstIP;
    m_srcPort = o.m_srcPort;
    m_dstPort = o.m_dstPort;
}

bool operator == (const FlowV4Id& a, const FlowV4Id& b){
    return (a.m_srcIP == b.m_srcIP) && (a.m_dstIP == b.m_dstIP) &&
        (a.m_srcPort == b.m_srcPort) && (a.m_dstPort == b.m_dstPort);
}

bool operator < (const FlowV4Id& a, const FlowV4Id& b){
    return std::tie(a.m_srcIP, a.m_dstIP, a.m_srcPort, a.m_dstPort) <
          std::tie(b.m_srcIP, b.m_dstIP, b.m_srcPort, b.m_dstPort);
}

const uint32_t Prime[5] = {2654435761U,246822519U,3266489917U,668265263U,374761393U};

const uint32_t prime[16] = {
    181, 5197, 1151, 137, 5569, 7699, 2887, 8753, 
    9323, 8963, 6053, 8893, 9377, 6577, 733, 3527
};

uint32_t
rotateLeft(uint32_t x, unsigned char bits)
{
    return (x << bits) | (x >> (32 - bits));
}

uint32_t 
FlowV4Id::hash(uint32_t seed){
    uint32_t result = prime[seed];

    result = rotateLeft(result + m_srcPort * Prime[2], 17) * Prime[3];
    result = rotateLeft(result + m_dstPort * Prime[4], 11) * Prime[0];
    result = rotateLeft(result + m_srcIP * Prime[3], 17) * Prime[1];
    result = rotateLeft(result + m_dstIP * Prime[0], 11) * Prime[4];

    return result;
}

PppHeader::PppHeader()
{
}

PppHeader::~PppHeader()
{
}

TypeId
PppHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::PppHeader")
                            .SetParent<Header>()
                            .SetGroupName("PointToPoint")
                            .AddConstructor<PppHeader>();
    return tid;
}

TypeId
PppHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
PppHeader::Print(std::ostream& os) const
{
    std::string proto;

    switch (m_protocol)
    {
    case 0x0021: /* IPv4 */
        proto = "IP (0x0021)";
        break;
    case 0x0057: /* IPv6 */
        proto = "IPv6 (0x0057)";
        break;
    default:
        NS_ASSERT_MSG(false, "PPP Protocol number not defined!");
    }
    os << "Point-to-Point Protocol: " << proto;
}

uint32_t
PppHeader::GetSerializedSize() const
{
    return 2;
}

void
PppHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteHtonU16(m_protocol);
}

uint32_t
PppHeader::Deserialize(Buffer::Iterator start)
{
    m_protocol = start.ReadNtohU16();
    return GetSerializedSize();
}

void
PppHeader::SetProtocol(uint16_t protocol)
{
    m_protocol = protocol;
}

uint16_t
PppHeader::GetProtocol() const
{
    return m_protocol;
}

} // namespace ns3
