#include "pfc-header.h"

#include "ns3/abort.h"
#include "ns3/assert.h"
#include "ns3/header.h"
#include "ns3/log.h"

#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("PfcHeader");

NS_OBJECT_ENSURE_REGISTERED(PfcHeader);

PfcHeader::PfcHeader()
{
    m_time = 0;
    m_queueSize = 0;
    m_queueIndex = 0;
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
    return;
}

uint32_t
PfcHeader::GetSerializedSize() const
{
    return 12;
}

void
PfcHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteHtonU32(m_time);
    start.WriteHtonU32(m_queueSize);
    start.WriteHtonU32(m_queueIndex);
}

uint32_t
PfcHeader::Deserialize(Buffer::Iterator start)
{
    m_time = start.ReadNtohU32();
    m_queueSize = start.ReadNtohU32();
    m_queueIndex = start.ReadNtohU32();
    return GetSerializedSize();
}

void
PfcHeader::SetTime(uint32_t time)
{
    m_time = time;
}

void
PfcHeader::SetQueueSize(uint32_t size)
{
    m_queueSize = size;
}

void
PfcHeader::SetQueueIndex(uint32_t index)
{
    m_queueIndex = index;
}

uint32_t
PfcHeader::GetTime() const
{
    return m_time;
}

uint32_t
PfcHeader::GetQueueSize() const
{
    return m_queueSize;
}

uint32_t
PfcHeader::GetQueueIndex() const
{
    return m_queueIndex;
}

} // namespace ns3
