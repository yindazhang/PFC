#include "pfc-header.h"

#include "ns3/abort.h"
#include "ns3/log.h"

#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("PfcHeader");

NS_OBJECT_ENSURE_REGISTERED(PfcHeader);

PfcHeader::PfcHeader()
    : PfcHeader(0, false)
{
}

PfcHeader::PfcHeader(uint8_t priority, bool pause)
    : m_priority(priority),
      m_pause(pause)
{
}

PfcHeader::~PfcHeader()
{
}

TypeId
PfcHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::PfcHeader")
                            .SetParent<Header>()
                            .SetGroupName("PointToPoint")
                            .AddConstructor<PfcHeader>();
    return tid;
}

TypeId
PfcHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
PfcHeader::Print(std::ostream& os) const
{
    os << "PFC " << (m_pause ? "pause" : "resume") << " priority=" << +m_priority;
}

uint32_t
PfcHeader::GetSerializedSize() const
{
    return SIZE;
}

void
PfcHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteHtonU16(OPCODE_PFC);
    start.WriteU8(m_priority);
    start.WriteU8(m_pause ? 1 : 0);
    start.WriteU8(0, SIZE - 4);
}

uint32_t
PfcHeader::Deserialize(Buffer::Iterator start)
{
    uint16_t opcode = start.ReadNtohU16();
    NS_ABORT_MSG_IF(opcode != OPCODE_PFC, "Not a PFC frame (opcode 0x" << std::hex << opcode << ")");
    m_priority = start.ReadU8();
    m_pause = start.ReadU8() != 0;
    start.Next(SIZE - 4);
    return SIZE;
}

uint8_t
PfcHeader::GetPriority() const
{
    return m_priority;
}

void
PfcHeader::SetPriority(uint8_t priority)
{
    m_priority = priority;
}

bool
PfcHeader::IsPause() const
{
    return m_pause;
}

void
PfcHeader::SetPause(bool pause)
{
    m_pause = pause;
}

} // namespace ns3
