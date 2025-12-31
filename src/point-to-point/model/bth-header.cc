#include "bth-header.h"

#include "ns3/abort.h"
#include "ns3/assert.h"
#include "ns3/header.h"
#include "ns3/log.h"

#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("BthHeader");

NS_OBJECT_ENSURE_REGISTERED(BthHeader);

BthHeader::BthHeader()
{
    m_opcode = 0;
    m_flags = 0;
    m_size = 0;
    m_id = 0;
    m_sequence = 0;
}

BthHeader::~BthHeader()
{
}

TypeId
BthHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::BthHeader")
                            .SetParent<Header>()
                            .SetGroupName("PointToPoint")
                            .AddConstructor<BthHeader>();
    return tid;
}

TypeId
BthHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
BthHeader::Print(std::ostream& os) const
{
    return;
}

uint32_t
BthHeader::GetSerializedSize() const
{
    return 12;
}

void
BthHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteU8(m_opcode);
    start.WriteU8(m_flags);
    start.WriteHtonU16(m_size);
    start.WriteHtonU32(m_id);
    start.WriteHtonU32(m_sequence);
}

uint32_t
BthHeader::Deserialize(Buffer::Iterator start)
{
    m_opcode = start.ReadU8();
    m_flags = start.ReadU8();
    m_size = start.ReadNtohU16();
    m_id = start.ReadNtohU32();
    m_sequence = start.ReadNtohU32();
    return GetSerializedSize();
}

void
BthHeader::SetOpcode(uint8_t opcode)
{
    m_opcode = opcode;
}

uint8_t
BthHeader::GetOpcode()
{
    return m_opcode;
}

uint8_t
BthHeader::GetCNP()
{
    return m_flags & 0x01;
}

void
BthHeader::SetCNP()
{
    m_flags |= 0x01;
}

uint8_t
BthHeader::GetACK()
{
    return (m_flags >> 1) & 0x01;
}

void
BthHeader::SetACK()
{
    m_flags |= (0x01 << 1);
}

uint8_t
BthHeader::GetNACK()
{
    return (m_flags >> 2) & 0x01;
}

void
BthHeader::SetNACK()
{
    m_flags |= (0x01 << 2);
}

uint16_t
BthHeader::GetSize()
{
    return m_size;
}

void
BthHeader::SetSize(uint16_t size)
{
    m_size = size;
}

uint32_t
BthHeader::GetId()
{
    return m_id;
}

void
BthHeader::SetId(uint32_t id)
{
    m_id = id;
}

uint32_t
BthHeader::GetSequence()
{
    return m_sequence;
}

void
BthHeader::SetSequence(uint32_t sequence)
{
    m_sequence = sequence;
}


} // namespace ns3
