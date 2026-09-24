#include "srv6-header.h"

#include "ns3/abort.h"
#include "ns3/assert.h"
#include "ns3/log.h"

#include <iomanip>
#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("Srv6Header");

NS_OBJECT_ENSURE_REGISTERED(Srv6RoutingHeader);

/* ---------------------------------------------------------------------------
 * Srv6Sid
 * ------------------------------------------------------------------------- */

Srv6Sid::Srv6Sid()
    : m_locator(0)
{
    for (auto& u : m_usid)
    {
        u = END_OF_CARRIER;
    }
}

Srv6Sid::Srv6Sid(uint32_t locator, const std::vector<uint16_t>& usids)
    : m_locator(locator)
{
    SetUsids(usids);
}

uint16_t
Srv6Sid::GetUsid(uint8_t i) const
{
    NS_ASSERT(i < NUM_USIDS);
    return m_usid[i];
}

void
Srv6Sid::SetUsid(uint8_t i, uint16_t usid)
{
    NS_ASSERT(i < NUM_USIDS);
    m_usid[i] = usid;
}

std::vector<uint16_t>
Srv6Sid::GetUsids() const
{
    std::vector<uint16_t> ret;
    for (uint8_t i = 0; i < NUM_USIDS && m_usid[i] != END_OF_CARRIER; i++)
    {
        ret.push_back(m_usid[i]);
    }
    return ret;
}

void
Srv6Sid::SetUsids(const std::vector<uint16_t>& usids)
{
    NS_ASSERT_MSG(usids.size() <= NUM_USIDS,
                  "Too many uSIDs (" << usids.size() << ") for one SID (max 6)");
    for (uint8_t i = 0; i < NUM_USIDS; i++)
    {
        m_usid[i] = i < usids.size() ? usids[i] : END_OF_CARRIER;
        NS_ASSERT_MSG(i >= usids.size() || usids[i] != END_OF_CARRIER,
                      "uSID 0x0000 is reserved for End-of-Carrier");
    }
}

bool
Srv6Sid::PushUsid(uint16_t usid)
{
    NS_ASSERT_MSG(usid != END_OF_CARRIER, "uSID 0x0000 is reserved for End-of-Carrier");
    for (auto& u : m_usid)
    {
        if (u == END_OF_CARRIER)
        {
            u = usid;
            return true;
        }
    }
    return false;
}

uint8_t
Srv6Sid::GetNumUsids() const
{
    return GetUsids().size();
}

bool
Srv6Sid::Matches(uint32_t locator, uint16_t usid) const
{
    return m_locator == locator && m_usid[0] == usid;
}

void
Srv6Sid::Shift()
{
    for (uint8_t i = 0; i + 1 < NUM_USIDS; i++)
    {
        m_usid[i] = m_usid[i + 1];
    }
    m_usid[NUM_USIDS - 1] = END_OF_CARRIER;
}

void
Srv6Sid::Serialize(Buffer::Iterator& start) const
{
    start.WriteHtonU32(m_locator);
    for (uint8_t i = 0; i < NUM_USIDS; i++)
    {
        start.WriteHtonU16(m_usid[i]);
    }
}

void
Srv6Sid::Deserialize(Buffer::Iterator& start)
{
    m_locator = start.ReadNtohU32();
    for (uint8_t i = 0; i < NUM_USIDS; i++)
    {
        m_usid[i] = start.ReadNtohU16();
    }
}

void
Srv6Sid::Print(std::ostream& os) const
{
    std::ios_base::fmtflags f = os.flags();
    os << std::hex << std::setfill('0') << std::setw(8) << m_locator;
    for (uint8_t i = 0; i < NUM_USIDS; i++)
    {
        os << (i ? ":" : "|") << std::setw(4) << m_usid[i];
    }
    os.flags(f);
    os << std::setfill(' ');
}

bool
Srv6Sid::operator==(const Srv6Sid& o) const
{
    if (m_locator != o.m_locator)
    {
        return false;
    }
    for (uint8_t i = 0; i < NUM_USIDS; i++)
    {
        if (m_usid[i] != o.m_usid[i])
        {
            return false;
        }
    }
    return true;
}

std::ostream&
operator<<(std::ostream& os, const Srv6Sid& sid)
{
    sid.Print(os);
    return os;
}

/* ---------------------------------------------------------------------------
 * Srv6RoutingHeader
 * ------------------------------------------------------------------------- */

Srv6RoutingHeader::Srv6RoutingHeader()
{
    m_nextHeader = IPV6_NEXT_HEADER_UDP;
    m_segmentsLeft = 0;
    m_flags = 0;
    m_tag = 0;
}

Srv6RoutingHeader::~Srv6RoutingHeader()
{
}

TypeId
Srv6RoutingHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::Srv6RoutingHeader")
                            .SetParent<Header>()
                            .SetGroupName("PointToPoint")
                            .AddConstructor<Srv6RoutingHeader>();
    return tid;
}

TypeId
Srv6RoutingHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint8_t
Srv6RoutingHeader::GetLastEntry() const
{
    return m_segments.empty() ? 0 : m_segments.size() - 1;
}

uint8_t
Srv6RoutingHeader::GetHdrExtLen() const
{
    // In 8-octet units, not including the first 8 octets
    return (m_segments.size() * Srv6Sid::SIZE) / 8;
}

void
Srv6RoutingHeader::SetSegmentList(const std::vector<Srv6Sid>& segments)
{
    NS_ASSERT_MSG(!segments.empty() && segments.size() <= 128, "Invalid SRH segment count");
    m_segments = segments;
}

void
Srv6RoutingHeader::SetPath(const std::vector<Srv6Sid>& path)
{
    NS_ASSERT_MSG(!path.empty() && path.size() <= 128, "Invalid SRv6 path length");
    // Segment List[0] = last segment of the path
    m_segments.assign(path.rbegin(), path.rend());
    m_segmentsLeft = path.size() - 1;
}

const Srv6Sid&
Srv6RoutingHeader::GetActiveSid() const
{
    NS_ASSERT_MSG(m_segmentsLeft < m_segments.size(), "Segments Left beyond Last Entry");
    return m_segments[m_segmentsLeft];
}

bool
Srv6RoutingHeader::Match(uint32_t locator, uint16_t usid) const
{
    return GetActiveSid().Matches(locator, usid);
}

bool
Srv6RoutingHeader::Shift()
{
    NS_ASSERT_MSG(m_segmentsLeft < m_segments.size(), "Segments Left beyond Last Entry");
    Srv6Sid& sid = m_segments[m_segmentsLeft];
    sid.Shift();
    if (!sid.IsEndOfCarrier())
    {
        return true;
    }
    // Active SID exhausted: continue with the next segment, if any
    if (m_segmentsLeft == 0)
    {
        return false;
    }
    m_segmentsLeft--;
    return true;
}

void
Srv6RoutingHeader::Print(std::ostream& os) const
{
    os << "SRH nh=" << +m_nextHeader << " len=" << +GetHdrExtLen() << " type=" << +ROUTING_TYPE_SRH
       << " sl=" << +m_segmentsLeft << " le=" << +GetLastEntry() << " flags=0x" << std::hex
       << +m_flags << " tag=0x" << m_tag << std::dec;
    for (size_t i = 0; i < m_segments.size(); i++)
    {
        os << " [" << i << "]=" << m_segments[i];
    }
}

uint32_t
Srv6RoutingHeader::GetSerializedSize() const
{
    return 8 + m_segments.size() * Srv6Sid::SIZE;
}

void
Srv6RoutingHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteU8(m_nextHeader);
    start.WriteU8(GetHdrExtLen());
    start.WriteU8(ROUTING_TYPE_SRH);
    start.WriteU8(m_segmentsLeft);
    start.WriteU8(GetLastEntry());
    start.WriteU8(m_flags);
    start.WriteHtonU16(m_tag);
    for (const auto& seg : m_segments)
    {
        seg.Serialize(start);
    }
}

uint32_t
Srv6RoutingHeader::Deserialize(Buffer::Iterator start)
{
    m_nextHeader = start.ReadU8();
    uint8_t hdrExtLen = start.ReadU8();
    uint8_t routingType = start.ReadU8();
    NS_ABORT_MSG_IF(routingType != ROUTING_TYPE_SRH, "Not an SRH (routing type " << +routingType << ")");
    m_segmentsLeft = start.ReadU8();
    uint8_t lastEntry = start.ReadU8();
    m_flags = start.ReadU8();
    m_tag = start.ReadNtohU16();

    uint32_t totalLen = 8 + uint32_t(hdrExtLen) * 8;
    uint32_t segLen = (uint32_t(lastEntry) + 1) * Srv6Sid::SIZE;
    NS_ABORT_MSG_IF(8 + segLen != totalLen, "SRH TLVs are not supported");

    m_segments.assign(lastEntry + 1, Srv6Sid());
    for (auto& seg : m_segments)
    {
        seg.Deserialize(start);
    }
    return totalLen;
}

} // namespace ns3
