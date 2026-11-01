#ifndef INT_HEADER_H
#define INT_HEADER_H

#include "ns3/header.h"

namespace ns3
{

/**
 * In-band network telemetry (INT): the low-priority (data) egress queue length
 * of the switch that added it: added by a spine (T1) switch at dequeue and
 * removed by the next switch on ingress (see SwitchNode::SetIntVersion).
 *
 * Carried as an 8-byte IPv6 extension header directly after the IPv6 header
 * (IPv6 Next Header = IPV6_NEXT_HEADER_INT), so the rest of the chain (SRH,
 * UDP, ...) stays intact: the inserting switch copies the IPv6 Next Header
 * into this header, and the extracting switch copies it back.
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |  Next Header  | Hdr Ext Len=0 |    Version    |   Reserved    |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                 Low-priority queue length (bytes)             |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
class IntHeader : public Header
{
public:
    static const uint8_t IPV6_NEXT_HEADER_INT = 253; //!< RFC 3692 experimental value
    static const uint32_t SIZE = 8;
    static const uint8_t VERSION_QUEUE_LENGTH = 1; //!< v1: low-priority queue length only
    static const uint8_t VERSION_MAX = VERSION_QUEUE_LENGTH;

    IntHeader();
    ~IntHeader() override;

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize() const override;

    /** Protocol of the header that follows this one (the original IPv6 Next Header). */
    uint8_t GetNextHeader() const;
    void SetNextHeader(uint8_t nextHeader);

    /** Telemetry format (VERSION_*). */
    uint8_t GetVersion() const;
    void SetVersion(uint8_t version);

    /** Low-priority egress queue length, in bytes. */
    uint32_t GetQueueLength() const;
    void SetQueueLength(uint32_t bytes);

private:
    uint8_t m_nextHeader{0};
    uint8_t m_version{VERSION_QUEUE_LENGTH};
    uint32_t m_queueLength{0};
};

} // namespace ns3

#endif /* INT_HEADER_H */
