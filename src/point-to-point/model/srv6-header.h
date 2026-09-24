#ifndef SRV6_HEADER_H
#define SRV6_HEADER_H

#include "ns3/header.h"

#include <vector>

namespace ns3
{

/*
 * SRv6 source routing used by MRC (RFC 8986 / RFC 9800 / RFC 8754).
 *
 * MRC encodes an explicit path as compressed SRv6 SIDs using the NEXT-CSID
 * ("uSID") flavor. Each 128-bit SID is a 32-bit locator followed by six
 * 16-bit uSIDs; unused uSID slots hold the End-of-Carrier value 0x0000:
 *
 *   |<-- locator (32b) -->|usid[0]|usid[1]|usid[2]|usid[3]|usid[4]|usid[5]|
 *
 * usid[0] is the active uSID. A node whose (locator, uSID) matches the
 * active SID shifts the uSIDs left by one slot and forwards the packet. When
 * the active uSID becomes End-of-Carrier, the next SID in the segment list
 * becomes active (if Segments Left > 0), otherwise the packet has reached
 * its egress.
 *
 * Simulation note: in real SRv6 the active SID lives in the IPv6 DA and only
 * the DA is shifted. Here the SRH carries the full path and is shifted in
 * place, so the SRH is self-contained and needs no IPv6 header.
 */

/** One 128-bit compressed SRv6 SID: 32-bit locator + six 16-bit uSIDs. */
class Srv6Sid
{
public:
    static const uint8_t SIZE = 16;
    static const uint8_t NUM_USIDS = 6;
    static const uint16_t END_OF_CARRIER = 0x0000;

    Srv6Sid();
    Srv6Sid(uint32_t locator, const std::vector<uint16_t>& usids);

    uint32_t GetLocator() const { return m_locator; }
    void SetLocator(uint32_t locator) { m_locator = locator; }

    /** uSID in slot \p i (0 = active); End-of-Carrier if unused. */
    uint16_t GetUsid(uint8_t i) const;
    void SetUsid(uint8_t i, uint16_t usid);

    /** uSIDs from the active one up to (not including) End-of-Carrier. */
    std::vector<uint16_t> GetUsids() const;
    /** Replace the uSID list; the remaining slots are set to End-of-Carrier. */
    void SetUsids(const std::vector<uint16_t>& usids);
    /** Append a uSID in the first End-of-Carrier slot; false if full. */
    bool PushUsid(uint16_t usid);
    uint8_t GetNumUsids() const;

    uint16_t GetActiveUsid() const { return m_usid[0]; }
    uint16_t GetNextUsid() const { return m_usid[1]; }
    bool IsEndOfCarrier() const { return m_usid[0] == END_OF_CARRIER; }

    /** True if (locator, active uSID) equals (\p locator, \p usid), i.e. a local uSID hit. */
    bool Matches(uint32_t locator, uint16_t usid) const;

    /** NEXT-CSID shift: drop the active uSID, pad the tail with End-of-Carrier. */
    void Shift();

    void Serialize(Buffer::Iterator& start) const;
    void Deserialize(Buffer::Iterator& start);
    void Print(std::ostream& os) const;

    bool operator==(const Srv6Sid& o) const;

private:
    uint32_t m_locator;
    uint16_t m_usid[NUM_USIDS];
};

std::ostream& operator<<(std::ostream& os, const Srv6Sid& sid);

/**
 * IPv6 Segment Routing Header (RFC 8754) carrying SRv6 SIDs:
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  | Next Header   |  Hdr Ext Len  | Routing Type=4| Segments Left |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |  Last Entry   |     Flags     |              Tag              |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |              Segment List[0] (128-bit SRv6 SID)               |
 *  |                              ...                              |
 *  |              Segment List[n] (128-bit SRv6 SID)               |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Segment List[0] is the LAST segment of the path (reverse order), and
 * Segment List[Segments Left] is the active SID. TLVs are not supported.
 */
class Srv6RoutingHeader : public Header
{
public:
    static const uint8_t IPV6_NEXT_HEADER_ROUTING = 43;
    static const uint8_t IPV6_NEXT_HEADER_IPV6 = 41; //!< IPv6-in-IPv6 inner packet
    static const uint8_t IPV6_NEXT_HEADER_UDP = 17;
    static const uint8_t ROUTING_TYPE_SRH = 4;

    Srv6RoutingHeader();
    ~Srv6RoutingHeader() override;

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize() const override;

    uint8_t GetNextHeader() const { return m_nextHeader; }
    void SetNextHeader(uint8_t nh) { m_nextHeader = nh; }

    uint8_t GetSegmentsLeft() const { return m_segmentsLeft; }
    void SetSegmentsLeft(uint8_t sl) { m_segmentsLeft = sl; }

    uint8_t GetLastEntry() const;
    uint8_t GetHdrExtLen() const;

    uint8_t GetFlags() const { return m_flags; }
    void SetFlags(uint8_t flags) { m_flags = flags; }

    uint16_t GetTag() const { return m_tag; }
    void SetTag(uint16_t tag) { m_tag = tag; }

    /** Segment list in wire order (index 0 = last segment of the path). */
    const std::vector<Srv6Sid>& GetSegmentList() const { return m_segments; }
    void SetSegmentList(const std::vector<Srv6Sid>& segments);

    /** Build the segment list from SIDs given in path order; the first SID becomes active. */
    void SetPath(const std::vector<Srv6Sid>& path);

    /** The active SID, Segment List[Segments Left]. */
    const Srv6Sid& GetActiveSid() const;
    uint16_t GetActiveUsid() const { return GetActiveSid().GetActiveUsid(); }

    /** True if the active SID's (locator, active uSID) equals (\p locator, \p usid). */
    bool Match(uint32_t locator, uint16_t usid) const;

    /**
     * NEXT-CSID End behavior: consume the active uSID. When the active SID runs
     * out of uSIDs, move to the next segment (Segments Left - 1).
     * \return true if the packet must be forwarded on the new active uSID,
     *         false if this node is the final endpoint of the path.
     */
    bool Shift();

private:
    uint8_t m_nextHeader;
    uint8_t m_segmentsLeft;
    uint8_t m_flags;
    uint16_t m_tag;
    std::vector<Srv6Sid> m_segments;
};

} // namespace ns3

#endif /* SRV6_HEADER_H */
