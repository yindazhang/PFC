#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include "ns3/ipv6-address.h"
#include "ns3/node.h"
#include "ns3/random-variable-stream.h"

#include "point-to-point-net-device.h"

#include <unordered_map>

namespace ns3
{

class Packet;
class Address;
class Ipv6Header;
class Srv6RoutingHeader;

/**
 * Output-queued IPv6 switch with a shared-buffer MMU. IPv4 is not supported.
 *
 * Identity: a switch is (plane, switchId). In SRv6 SIDs the plane is the
 * 32-bit locator and the switchId is the 16-bit uSID.
 *
 * Forwarding:
 *   - SRv6 (IPv6 next header = routing, type 4): if the active SID is
 *     (plane, switchId) the switch consumes its uSID (Srv6RoutingHeader::Shift).
 *     While the path has an active uSID, the packet is forwarded on the SID
 *     table towards (locator, uSID). Once the path is exhausted, forwarding
 *     falls back to the IPv6 destination.
 *   - Otherwise: exact match on the IPv6 destination.
 * Among multiple next hops, ECMP hashes only IPv6 header fields (src, dst,
 * flow label, next header), never transport ports.
 *
 * Buffer model (ingress accounting, per ingress port):
 *   - reserved: PORT_RESERVED_BYTES guaranteed to every port;
 *   - shared:   a pool shared by all ports, admitted with a dynamic threshold
 *               (alpha = 1): a port may use at most the free shared space;
 *   - headroom: per-port space for bytes still in flight after a PFC pause.
 * Total buffer is BUFFER_BYTES_PER_GBPS bytes per Gbps of each port's rate.
 *
 * PFC: a port is paused once it uses headroom or reaches the shared threshold,
 * and resumed when headroom is empty and it is PFC_RESUME_OFFSET below threshold.
 *
 * ECN: RED on egress occupancy with Kmin/Kmax proportional to the port buffer
 * and marking probability up to ECN_PMAX; only ECN-capable packets are marked.
 * Marking happens at dequeue (default: on the queue left behind, so the
 * signal is not delayed by the queue itself) or at enqueue (SetEcnAtDequeue).
 *
 * Trimming (SetTrimEnabled): a low-priority RoCE packet that fails admission
 * is cut to its headers (IPv6 [+ SRH] + UDP + BTH with extensions), marked
 * with the BTH trimmed bit and DSCP_HIGH_PRIORITY, and forwarded as high
 * priority so the receiver can NACK it. When disabled, such packets are dropped.
 *
 * Priorities (see PointToPointQueue): the MMU, PFC and ECN above apply only to
 * PRIORITY_LOW (data). PRIORITY_HIGH (ACK/SACK/NACK/trimmed/...) bypasses
 * admission and accounting, is never paused or marked, and is assumed never
 * to overflow its egress queue.
 */
class SwitchNode : public Node
{
public:
    /**
     * \brief Get the type ID.
     * \return the object TypeId
     */
    static TypeId GetTypeId();

    SwitchNode();
    ~SwitchNode() override;

    /**
     * \brief Associate a NetDevice to this node and size its buffers.
     *
     * \param device NetDevice to associate to this node.
     * \returns the index of the NetDevice into the Node's list of NetDevice.
     */
    uint32_t AddDevice(Ptr<NetDevice> device) override;

    /**
     * \brief Receive callback installed on every device.
     * \returns true if the packet has been forwarded.
     */
    bool ReceiveFromDevice(Ptr<NetDevice> device,
                           Ptr<const Packet> packet,
                           uint16_t protocol,
                           const Address& from);

    /** Add device \p ifIndex as an (ECMP) next hop towards \p dst. */
    void AddRoute(Ipv6Address dst, uint32_t ifIndex);
    /** Add device \p ifIndex as an (ECMP) next hop towards the owner of (\p locator, \p usid). */
    void AddSidRoute(uint32_t locator, uint16_t usid, uint32_t ifIndex);

    void SetEcmpSeed(uint32_t seed);
    /** Enable/disable ECN marking on egress device \p ifIndex (MRC disables it on the last hop). */
    void SetEcnEnabled(uint32_t ifIndex, bool enabled);
    /** Mark ECN when a packet leaves the egress queue (default) or when it enters it. */
    void SetEcnAtDequeue(bool atDequeue);
    void SetPfcEnabled(bool enabled);
    /** Trim (instead of drop) low-priority packets that fail admission. Default off. */
    void SetTrimEnabled(bool enabled);

    /**
     * Identity of this switch, matched against SRv6 SIDs.
     * \param plane    fabric plane; equals the SID locator
     * \param switchId switch within the plane; equals the uSID
     */
    void SetSwitchId(uint32_t plane, uint16_t switchId);
    uint32_t GetPlane() const;
    uint16_t GetSwitchId() const;

    /** Low-priority (data) bytes sent on device \p ifIndex. */
    uint64_t GetTxBytes(uint32_t ifIndex) const;
    uint64_t GetDropCount() const { return m_dropCount; }
    uint64_t GetTrimCount() const { return m_trimCount; }
    uint64_t GetEcnMarkCount() const { return m_ecnMarkCount; }

    /** Admission, routing, buffer accounting, PFC/ECN and forwarding. */
    bool IngressPipeline(Ptr<Packet> packet, uint16_t protocol, Ptr<PointToPointNetDevice> inPort);
    /** Release MMU buffer when a packet leaves \p outPort; may resume PFC. */
    Ptr<Packet> EgressPipeline(Ptr<Packet> packet,
                               uint16_t protocol,
                               Ptr<PointToPointNetDevice> outPort);

protected:
    static const uint16_t PROTOCOL_IPV4 = 0x0800;
    static const uint16_t PROTOCOL_IPV6 = 0x86DD;
    static const uint16_t PROTOCOL_PFC = 0x8808;

    // Buffer configuration
    static const int32_t PORT_RESERVED_BYTES = 10000;  //!< Reserved bytes per ingress port
    static const int32_t PFC_RESUME_OFFSET = 10000;    //!< PFC resume hysteresis (bytes)
    static const int32_t BUFFER_BYTES_PER_GBPS = 5000; //!< Buffer per Gbps of port rate
    static const int32_t MAX_PACKET_SIZE = 9000;       //!< Jumbo frame, for headroom sizing
    static const int32_t HEADROOM_EXTRA_PACKETS = 3;   //!< Packets beyond one RTT in headroom

    // ECN (RED) configuration, relative to the port buffer
    static constexpr double ECN_KMIN_RATIO = 0.1;
    static constexpr double ECN_KMAX_RATIO = 0.4;
    static constexpr double ECN_PMAX = 0.2;

    /** Per-port MMU, ECN and PFC state (low priority only). */
    struct PortState
    {
        int32_t headroomSize{0}; //!< Headroom allocated to this ingress port
        int32_t headroomUsed{0}; //!< Headroom bytes in use (as ingress)
        int32_t ingressUsed{0};  //!< Reserved + shared bytes in use (as ingress)
        int32_t egressUsed{0};   //!< Bytes queued for transmission (as egress)
        int32_t ecnKmin{0};
        int32_t ecnKmax{0};
        bool paused{false};      //!< PFC pause sent to the upstream of this port
        bool ecnEnabled{true};   //!< ECN marking as egress
        uint64_t txBytes{0};     //!< Low-priority bytes sent (as egress)
    };

    /** Shared bytes used by \p port: ingress bytes beyond its reservation. */
    static int32_t GetPortSharedUsed(const PortState& port);
    /** Dynamic shared-buffer threshold (alpha = 1): the free shared space. */
    int32_t GetSharedThreshold() const;

    bool ShouldMarkEcn(const PortState& port);
    bool ShouldPause(const PortState& port) const;
    bool ShouldResume(const PortState& port) const;
    /** Send a PFC pause/resume for PRIORITY_LOW upstream of \p port. */
    void SendPfc(Ptr<PointToPointNetDevice> port, bool pause);

    /**
     * Build a header-only packet from a RoCE packet (IPv6 header already
     * removed) with the BTH trimmed bit set. On success also sets
     * DSCP_HIGH_PRIORITY and the new payload length in \p ipv6Header.
     * \p packet is consumed (its headers are removed) and must be dropped.
     * \return nullptr if the packet is not trimmable (not UDP/RoCE).
     */
    Ptr<Packet> TrimPacket(Ptr<Packet> packet, Ipv6Header& ipv6Header) const;

    /** ECMP hash over IPv6 header fields only (src, dst, flow label, next header). */
    uint32_t EcmpHash(const Ipv6Header& header) const;

    /**
     * Next-hop candidates for a packet (IPv6 header already removed). Applies
     * SRv6 End processing to the SRH in \p packet when present.
     * \return nullptr if there is no route.
     */
    const std::vector<uint32_t>* LookupNextHops(Ptr<Packet> packet, const Ipv6Header& ipv6Header);

    static uint64_t SidKey(uint32_t locator, uint16_t usid)
    {
        return (uint64_t(locator) << 16) | usid;
    }

    uint32_t m_plane{0};
    uint16_t m_switchId{0};
    uint32_t m_ecmpSeed{0};
    bool m_pfcEnabled{false};
    bool m_trimEnabled{false};
    bool m_ecnAtDequeue{true};

    std::unordered_map<Ipv6Address, std::vector<uint32_t>, Ipv6AddressHash>
        m_routes; //!< dst -> next-hop device indices
    std::unordered_map<uint64_t, std::vector<uint32_t>>
        m_sidRoutes; //!< SidKey(locator, uSID) -> next-hop device indices

    std::unordered_map<Ptr<PointToPointNetDevice>, PortState> m_ports;

    int32_t m_sharedPoolSize{0}; //!< Buffer minus all reserved and headroom space
    int32_t m_sharedPoolUsed{0}; //!< Shared bytes in use, over all ingress ports

    uint64_t m_dropCount{0};
    uint64_t m_trimCount{0};
    uint64_t m_ecnMarkCount{0};

    // Value member on purpose: CreateObject would consume an automatic RNG
    // stream and shift every other random variable in the simulation.
    UniformRandomVariable m_ecnRand;
};

} // namespace ns3

#endif /* SWITCH_NODE_H */
