#ifndef PFC_HEADER_H
#define PFC_HEADER_H

#include "ns3/header.h"

namespace ns3
{

/**
 * Simplified Priority Flow Control (IEEE 802.1Qbb) MAC control frame.
 *
 * One frame pauses or resumes one priority class. A pause lasts until the
 * matching resume is received (no pause quanta / expiry).
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |        Opcode = 0x0101        |   Priority    | Pause (1/0)   |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |              Padding to the minimum Ethernet payload          |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
class PfcHeader : public Header
{
public:
    static const uint16_t OPCODE_PFC = 0x0101;
    static const uint32_t SIZE = 46; //!< Minimum Ethernet payload (64B frame)

    PfcHeader();
    PfcHeader(uint8_t priority, bool pause);
    ~PfcHeader() override;

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize() const override;

    /** Priority class this frame applies to. */
    uint8_t GetPriority() const;
    void SetPriority(uint8_t priority);

    /** true: pause the class; false: resume it. */
    bool IsPause() const;
    void SetPause(bool pause);

private:
    uint8_t m_priority;
    bool m_pause;
};

} // namespace ns3

#endif /* PFC_HEADER_H */
