/*
 * Copyright (c) 2008 University of Washington
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PPP_HEADER_H
#define PPP_HEADER_H

#include "ns3/header.h"

namespace ns3
{

#pragma pack(push, 1)
struct FlowV4Id
{
    uint32_t m_srcIP;
    uint32_t m_dstIP;
    uint16_t m_srcPort;
    uint16_t m_dstPort;

    FlowV4Id(uint32_t srcIP = 0, uint32_t dstIP = 0, uint16_t srcPort = 0, uint16_t dstPort = 0)
        : m_srcIP(srcIP), m_dstIP(dstIP), m_srcPort(srcPort), m_dstPort(dstPort)
    {};
    FlowV4Id(const FlowV4Id& flow);

    uint32_t hash(uint32_t seed = 0);
};
#pragma pack(pop)

bool operator == (const FlowV4Id& a, const FlowV4Id& b);
bool operator < (const FlowV4Id& a, const FlowV4Id& b);

/**
 * @ingroup point-to-point
 * @brief Packet header for PPP
 *
 * This class can be used to add a header to PPP packet.  Currently we do not
 * implement any of the state machine in \RFC{1661}, we just encapsulate the
 * inbound packet send it on.  The goal here is not really to implement the
 * point-to-point protocol, but to encapsulate our packets in a known protocol
 * so packet sniffers can parse them.
 *
 * if PPP is transmitted over a serial link, it will typically be framed in
 * some way derivative of IBM SDLC (HDLC) with all that that entails.
 * Thankfully, we don't have to deal with all of that -- we can use our own
 * protocol for getting bits across the serial link which we call an ns3
 * Packet.  What we do have to worry about is being able to capture PPP frames
 * which are understandable by Wireshark.  All this means is that we need to
 * teach the PcapWriter about the appropriate data link type (DLT_PPP = 9),
 * and we need to add a PPP header to each packet.  Since we are not using
 * framed PPP, this just means prepending the sixteen bit PPP protocol number
 * to the packet.  The ns-3 way to do this is via a class that inherits from
 * class Header.
 */
class PppHeader : public Header
{
  public:
    /**
     * @brief Construct a PPP header.
     */
    PppHeader();

    /**
     * @brief Destroy a PPP header.
     */
    ~PppHeader() override;

    /**
     * @brief Get the TypeId
     *
     * @return The TypeId for this class
     */
    static TypeId GetTypeId();

    /**
     * @brief Get the TypeId of the instance
     *
     * @return The TypeId for this instance
     */
    TypeId GetInstanceTypeId() const override;

    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize() const override;

    /**
     * @brief Set the protocol type carried by this PPP packet
     *
     * The type numbers to be used are defined in \RFC{3818}
     *
     * @param protocol the protocol type being carried
     */
    void SetProtocol(uint16_t protocol);

    /**
     * @brief Get the protocol type carried by this PPP packet
     *
     * The type numbers to be used are defined in \RFC{3818}
     *
     * @return the protocol type being carried
     */
    uint16_t GetProtocol() const;

  private:
    /**
     * @brief The PPP protocol type of the payload packet
     */
    uint16_t m_protocol;
    uint32_t m_padding4;
    uint64_t m_padding8;
};

} // namespace ns3

#endif /* PPP_HEADER_H */
