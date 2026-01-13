#include "bubble-header.h"

#include "ns3/abort.h"
#include "ns3/assert.h"
#include "ns3/header.h"
#include "ns3/log.h"

#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("BubbleHeader");

NS_OBJECT_ENSURE_REGISTERED(BubbleHeader);

BubbleHeader::BubbleHeader()
{
    m_bubbleRate = 0;
}

BubbleHeader::~BubbleHeader()
{
}

TypeId
BubbleHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::BubbleHeader")
                            .SetParent<Header>()
                            .SetGroupName("PointToPoint")
                            .AddConstructor<BubbleHeader>();
    return tid;
}

TypeId
BubbleHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
BubbleHeader::Print(std::ostream& os) const
{
    return;
}

uint32_t
BubbleHeader::GetSerializedSize() const
{
    return 1;
}

void
BubbleHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteU8(m_bubbleRate);
}

uint32_t
BubbleHeader::Deserialize(Buffer::Iterator start)
{
    m_bubbleRate = start.ReadU8();
    return GetSerializedSize();
}

void
BubbleHeader::SetBubbleRate(uint8_t rate)
{
    m_bubbleRate = rate;
}

uint8_t
BubbleHeader::GetBubbleRate() const
{
    return m_bubbleRate;
}

} // namespace ns3
