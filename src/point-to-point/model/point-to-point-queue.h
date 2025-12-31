#ifndef POINT_TO_POINT_QUEUE_H
#define POINT_TO_POINT_QUEUE_H

#include "ns3/drop-tail-queue.h"
#include "point-to-point-net-device.h"

#include <vector>

namespace ns3
{

class PointToPointQueue : public Queue<Packet>
{
public:
    /**
     * \brief Get the type ID.
     * \return the object TypeId
     */
    static TypeId GetTypeId();
    /**
     * \brief PointToPointQueue Constructor
     */
    PointToPointQueue();

    ~PointToPointQueue() override;

    bool Enqueue(Ptr<Packet> packet) override;
    Ptr<Packet> Dequeue() override;
    Ptr<Packet> Remove() override;
    Ptr<const Packet> Peek() const override;

    bool IsEmpty() const;
    uint32_t GetNBytes() const;

    void SetPauseFlag(uint32_t index, bool flag);
    bool GetPauseFlag(uint32_t index) const;

protected:
    std::vector<Ptr<DropTailQueue<Packet>>> m_queues;
    std::vector<bool> m_pauseFlags;

    static const uint8_t NUM_QUEUE = 4;
};

} // namespace ns3

#endif /* POINT_TO_POINT_QUEUE_H */
