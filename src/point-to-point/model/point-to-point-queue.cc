#include "point-to-point-queue.h"

#include "ns3/abort.h"
#include "ns3/enum.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/uinteger.h"

#include "ns3/ppp-header.h"

#include "point-to-point-net-device.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("PointToPointQueue");

NS_OBJECT_ENSURE_REGISTERED(PointToPointQueue);

TypeId
PointToPointQueue::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::PointToPointQueue")
            .SetParent<Queue<Packet>>()
            .SetGroupName("PointToPoint")
            .AddConstructor<PointToPointQueue>();
    return tid;
}

PointToPointQueue::PointToPointQueue()
{
    // Low-priority occupancy is bounded by the switch MMU; high priority is
    // assumed never to overflow its 16 MiB.
    for (uint32_t i = 0; i < NUM_PRIORITY; ++i)
    {
        m_queues.push_back(CreateObject<DropTailQueue<Packet>>());
        m_queues[i]->SetMaxSize(QueueSize("16MiB"));
        m_pauseFlags.push_back(false);
    }
}

PointToPointQueue::~PointToPointQueue() {}

bool
PointToPointQueue::Enqueue(Ptr<Packet> item)
{
    Priority priority = GetPriority(item);
    bool ret = m_queues[priority]->Enqueue(item);
    if (!ret)
    {
        NS_LOG_WARN("Queue overflow at priority " << priority << " ("
                  << m_queues[priority]->GetNBytes() << " bytes queued)");
    }
    return ret;
}

Ptr<Packet>
PointToPointQueue::Dequeue()
{
    // Strict priority: PRIORITY_HIGH (0) first; skip paused classes
    for (uint32_t i = 0; i < NUM_PRIORITY; ++i)
    {
        if (m_pauseFlags[i])
            continue;
        Ptr<Packet> ret = m_queues[i]->Dequeue();
        if(ret != nullptr)
            return ret;
    }
    return nullptr;
}

Ptr<Packet>
PointToPointQueue::Remove()
{
    NS_LOG_WARN("Remove in PointToPointQueue is not implemented now.");
    return nullptr;
}

Ptr<const Packet>
PointToPointQueue::Peek() const
{
    NS_LOG_WARN("Peek in PointToPointQueue is not implemented now.");
    return nullptr;
}

bool
PointToPointQueue::IsEmpty() const
{
    for (const auto& queue : m_queues)
        if(!queue->IsEmpty())
            return false;
    return true;
}

uint32_t
PointToPointQueue::GetNBytes() const
{
    uint32_t totalBytes = 0;
    for (const auto& queue : m_queues)
        totalBytes += queue->GetNBytes();
    return totalBytes;
}

uint32_t 
PointToPointQueue::GetNBytes(uint32_t index) const
{
    if(index >= NUM_PRIORITY){
        NS_ABORT_MSG("Invalid index in PointToPointQueue::GetNBytes " << index);
    }
    return m_queues[index]->GetNBytes();
}

void
PointToPointQueue::SetPauseFlag(uint32_t index, bool flag)
{
    if(index >= NUM_PRIORITY){
        NS_ABORT_MSG("Invalid index in PointToPointQueue::SetPauseFlag " << index);
    }
    NS_ABORT_MSG_IF(index == PRIORITY_HIGH && flag, "PRIORITY_HIGH must never be paused");
    m_pauseFlags[index] = flag;
}

PointToPointQueue::Priority
PointToPointQueue::GetPriority(const Ipv6Header& header)
{
    return header.GetDscp() == DSCP_HIGH_PRIORITY ? PRIORITY_HIGH : PRIORITY_LOW;
}

PointToPointQueue::Priority
PointToPointQueue::GetPriority(Ptr<const Packet> packet)
{
    PppHeader ppp;
    Ptr<Packet> copy = packet->Copy();
    copy->RemoveHeader(ppp);

    switch (ppp.GetProtocol())
    {
    case 0x0057: { // IPv6
        Ipv6Header ipv6;
        copy->PeekHeader(ipv6);
        return GetPriority(ipv6);
    }
    case 0x0021: // IPv4 (not supported by switches; treated as data)
        return PRIORITY_LOW;
    default: // PFC and other link-level control frames
        return PRIORITY_HIGH;
    }
}

bool
PointToPointQueue::GetPauseFlag(uint32_t index) const
{
    if(index >= NUM_PRIORITY){
        NS_ABORT_MSG("Invalid index in PointToPointQueue::GetPauseFlag " << index);
    }
    return m_pauseFlags[index];
}

} // namespace ns3
