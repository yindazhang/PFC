#include "int-header.h"

#include "ns3/abort.h"
#include "ns3/log.h"

#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("IntHeader");

NS_OBJECT_ENSURE_REGISTERED(IntHeader);

IntHeader::IntHeader()
{
}

IntHeader::~IntHeader()
{
}

TypeId
IntHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::IntHeader")
                            .SetParent<Header>()
                            .SetGroupName("PointToPoint")
                            .AddConstructor<IntHeader>();
    return tid;
}

TypeId
IntHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
IntHeader::Print(std::ostream& os) const
{
    os << "INT v" << +m_version << " next=" << +m_nextHeader << " qlen=" << m_queueLength;
}

uint32_t
IntHeader::GetSerializedSize() const
{
    return SIZE;
}

void
IntHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteU8(m_nextHeader);
    start.WriteU8(0); // Hdr Ext Len: 8-byte units beyond the first 8 bytes
    start.WriteU8(m_version);
    start.WriteU8(0);
    start.WriteHtonU32(m_queueLength);
}

uint32_t
IntHeader::Deserialize(Buffer::Iterator start)
{
    m_nextHeader = start.ReadU8();
    uint8_t extLen = start.ReadU8();
    NS_ABORT_MSG_IF(extLen != 0, "Unexpected INT header length " << +extLen);
    m_version = start.ReadU8();
    start.ReadU8();
    m_queueLength = start.ReadNtohU32();
    return SIZE;
}

uint8_t
IntHeader::GetNextHeader() const
{
    return m_nextHeader;
}

void
IntHeader::SetNextHeader(uint8_t nextHeader)
{
    m_nextHeader = nextHeader;
}

uint8_t
IntHeader::GetVersion() const
{
    return m_version;
}

void
IntHeader::SetVersion(uint8_t version)
{
    m_version = version;
}

uint32_t
IntHeader::GetQueueLength() const
{
    return m_queueLength;
}

void
IntHeader::SetQueueLength(uint32_t bytes)
{
    m_queueLength = bytes;
}

} // namespace ns3
