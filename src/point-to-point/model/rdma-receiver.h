#ifndef RDMA_RECEIVER_H
#define RDMA_RECEIVER_H

#include "bth-header.h"

#include "ns3/callback.h"
#include "ns3/ipv6-header.h"
#include "ns3/object.h"
#include "ns3/packet.h"

#include <map>
#include <vector>

namespace ns3
{

/**
 * MRC responder for one long-lived QP, installed directly at the destination host.
 *
 * Accepts data packets in any order within the Maximum PSN Range (MPR). The
 * last packet of each message (WRITE_LAST/ONLY_WITH_IMM) carries its id as
 * immediate data; messages complete in PSN order, once every packet up to
 * that one was received. Every data packet is answered with one reply:
 *   - SACK (MRC_SACK: SETH + CC_STATE) for new or duplicate data. It carries
 *     the cumulative ack (all PSNs below it received), a 64-bit bitmap of
 *     out-of-order arrivals above it, the reflected entropy value and
 *     congestion telemetry. With an empty bitmap it is a plain ACK.
 *   - NACK (MRC_NACK: NETH) for a trimmed packet (NACK_TRIMMED) or a PSN
 *     beyond the MPR window (NACK_OUT_OF_MPR).
 * Replies are high priority and have the BTH CNP (BECN) bit set when the data
 * was ECN CE-marked, and on every NACK.
 */
class RdmaReceiver : public Object
{
public:
    static TypeId GetTypeId();

    /** \param qpn QP number the sender uses as BTH DestQP. */
    explicit RdmaReceiver(uint32_t qpn = 0);
    ~RdmaReceiver() override;

    uint32_t GetQpn() const { return m_qpn; }

    /**
     * Process a data packet (starting with its IPv6 header).
     * \return the SACK/NACK to send back, or nullptr if the packet is not
     *         an MRC request for this QP.
     */
    Ptr<Packet> Receive(Ptr<Packet> packet);

    /** Invoked with the immediate data (message id) of every message, once fully received. */
    void SetMessageCallback(Callback<void, uint32_t> callback) { m_onMessage = callback; }

    /** Unique payload bytes received so far. */
    uint64_t GetReceivedBytes() const { return m_rcvdBytes; }

    /**
     * Strip IPv6 [+ SRH] + UDP (RoCE port) + BTH from \p packet.
     * \return false if the packet is not RoCE.
     */
    static bool ParseHeaders(Ptr<Packet> packet, Ipv6Header& ipv6Header, BthHeader& bthHeader);
    /** Non-destructive ParseHeaders(), e.g. to demultiplex on BTH DestQP/opcode. */
    static bool PeekHeaders(Ptr<const Packet> packet, Ipv6Header& ipv6Header, BthHeader& bthHeader);

protected:
    void NotifyConstructionCompleted() override;

private:
    static const uint32_t SACK_BITMAP_BITS = 64;
    static const uint32_t MPR_UNIT = 128; //!< SETH.mpr is advertised in units of 128 PSNs

    bool IsReceived(uint32_t psn) const;

    Ptr<Packet> BuildSack(const Ipv6Header& dataIp,
                          const BthHeader& dataBth,
                          bool ce,
                          bool duplicate = false) const;
    Ptr<Packet> BuildNack(const Ipv6Header& dataIp,
                          const BthHeader& dataBth,
                          uint32_t psn,
                          uint8_t code) const;
    /** Add BTH/UDP/IPv6 around a reply body; IPv6 addresses are swapped from the data. */
    Ptr<Packet> Encapsulate(BthHeader& replyBth, const Ipv6Header& dataIp) const;

    uint32_t m_qpn;
    uint32_t m_mpr;                //!< Receive window in PSNs (attribute)

    uint32_t m_expectedPsn{0}; //!< Cumulative ack: every PSN below it is received
    uint64_t m_rcvdBytes{0};
    std::vector<bool> m_received; //!< Ring over [m_expectedPsn, m_expectedPsn + m_mpr)
    std::map<uint32_t, uint32_t> m_lastPackets; //!< Last PSN -> id of a message not complete yet
    Callback<void, uint32_t> m_onMessage;
};

} // namespace ns3

#endif /* RDMA_RECEIVER_H */
