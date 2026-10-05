#ifndef POINT_TO_POINT_QUEUE_H
#define POINT_TO_POINT_QUEUE_H

#include "ns3/drop-tail-queue.h"
#include "ns3/ipv6-header.h"
#include "point-to-point-net-device.h"

#include <vector>

namespace ns3
{

/**
 * Two-class strict-priority queue. The class is taken from the packet headers:
 *   - PRIORITY_HIGH: control traffic (ACK/SACK/NACK/CNP/probes, trimmed
 *     packets), i.e. IPv6 packets with DSCP_HIGH_PRIORITY, and PFC frames;
 *   - PRIORITY_LOW:  everything else (data, including retransmissions).
 * Only PRIORITY_LOW can be paused by PFC; PRIORITY_HIGH is always served first.
 */
class PointToPointQueue : public Queue<Packet>
{
public:
    enum Priority : uint32_t
    {
        PRIORITY_HIGH = 0,
        PRIORITY_LOW = 1,
        NUM_PRIORITY = 2,
    };

    /** IPv6 DSCP that senders (and trimming switches) use for high-priority packets. */
    static const Ipv6Header::DscpType DSCP_HIGH_PRIORITY = Ipv6Header::DSCP_CS6;

    /**
     * The 2-bit IPv6 ECN field is used as two independent mark bits instead of
     * the RFC 3168 codepoints: senders send 00, switches only set bits, and
     * every low-priority (data) packet is ECN-capable, so no codepoint is spent
     * on Not-ECT. High-priority packets are never marked.
     */
    static const uint8_t ECN_MARK_QUEUE = 0x02; //!< ECN: congestion-control signal
    static const uint8_t ECN_MARK_LB = 0x01;    //!< Load-balancing signal: MRC path switching

    /** Whether \p header carries ECN mark bit \p mark. */
    static bool HasEcnMark(const Ipv6Header& header, uint8_t mark)
    {
        return (header.GetEcn() & mark) != 0;
    }

    /** Set ECN mark bit \p mark on \p header, keeping the other bit. */
    static void SetEcnMark(Ipv6Header& header, uint8_t mark)
    {
        header.SetEcn(Ipv6Header::EcnType(header.GetEcn() | mark));
    }

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
    uint32_t GetNBytes(uint32_t index) const;

    void SetPauseFlag(uint32_t index, bool flag);
    bool GetPauseFlag(uint32_t index) const;

    /** Priority of an IPv6 packet from its header. */
    static Priority GetPriority(const Ipv6Header& header);
    /** Priority of a packet that starts with a PPP header (as queued by the device). */
    static Priority GetPriority(Ptr<const Packet> packet);

protected:
    std::vector<Ptr<DropTailQueue<Packet>>> m_queues;
    std::vector<bool> m_pauseFlags;
};

} // namespace ns3

#endif /* POINT_TO_POINT_QUEUE_H */
