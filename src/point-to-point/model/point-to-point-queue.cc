#include "point-to-point-queue.h"

#include "ns3/abort.h"
#include "ns3/enum.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/uinteger.h"

#include "ns3/socket.h"
#include "ns3/ipv4-header.h"
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
    for(uint32_t i = 0; i < NUM_QUEUE; ++i){
        m_queues.push_back(CreateObject<DropTailQueue<Packet>>());
        m_queues[i]->SetMaxSize(QueueSize("16MiB"));
        m_pauseFlags.push_back(false);
    }
}

PointToPointQueue::~PointToPointQueue() {}

bool
PointToPointQueue::Enqueue(Ptr<Packet> item)
{
    int priority = 0;
    SocketPriorityTag socketPriorityTag;
    if(item->PeekPacketTag(socketPriorityTag)){
        priority = socketPriorityTag.GetPriority();
        if(priority < 0 || priority >= NUM_QUEUE){
            NS_ABORT_MSG("Invalid priority in PointToPointQueue::Enqueue " << priority);
        }
    }

    if(priority == 1 && m_queues[priority]->GetNPackets() >= 2){
        // Limit the number of bubble packets in high priority queue
        return false;
    }

    bool ret = m_queues[priority]->Enqueue(item);
    if(!ret){
        std::cout << "Error in buffer " << priority << std::endl;
        std::cout << "Buffer size " << m_queues[priority]->GetNBytes() << std::endl;
    }
    return ret;
}

Ptr<Packet>
PointToPointQueue::Dequeue()
{
    for(uint32_t i = 0; i < NUM_QUEUE; ++i){
        if(m_pauseFlags[i])
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
    std::cout << "Remove in PointToPointQueue is not implemented now." << std::endl;
    return nullptr;
}

Ptr<const Packet>
PointToPointQueue::Peek() const
{
    std::cout << "Peek in PointToPointQueue is not implemented now." << std::endl;
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
    if(index >= NUM_QUEUE){
        NS_ABORT_MSG("Invalid index in PointToPointQueue::GetNBytes " << index);
    }
    return m_queues[index]->GetNBytes();
}

void
PointToPointQueue::SetPauseFlag(uint32_t index, bool flag)
{
    if(index >= NUM_QUEUE){
        NS_ABORT_MSG("Invalid index in PointToPointQueue::SetPauseFlag " << index);
    }
    m_pauseFlags[index] = flag;
}

bool
PointToPointQueue::GetPauseFlag(uint32_t index) const
{
    if(index >= NUM_QUEUE){
        NS_ABORT_MSG("Invalid index in PointToPointQueue::GetPauseFlag " << index);
    }
    return m_pauseFlags[index];
}

} // namespace ns3
