#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include "ns3/node.h"
#include "ns3/random-variable-stream.h"

#include "point-to-point-net-device.h"

#include <unordered_map>

namespace ns3
{

class Application;
class Packet;
class Address;
class Time;

class SwitchNode : public Node
{
    
public:

    /**
     * \brief Get the type ID.
     * \return the object TypeId
     */
    static TypeId GetTypeId();

    SwitchNode();
    virtual ~SwitchNode();

    /**
     * \brief Associate a NetDevice to this node.
     *
     * \param device NetDevice to associate to this node.
     * \returns the index of the NetDevice into the Node's list of
     *          NetDevice.
     */
    uint32_t AddDevice(Ptr<NetDevice> device) override;

    /**
     * \brief Receive a packet from a device.
     * \param device the device
     * \param packet the packet
     * \param protocol the protocol
     * \param from the sender
     * \returns true if the packet has been delivered to a protocol handler.
     */
    bool ReceiveFromDevice(Ptr<NetDevice> device,
                                Ptr<const Packet> packet,
                                uint16_t protocol,
                                const Address& from);

    void AddHostRouteTo(uint32_t dst, uint32_t devId);

    void SetECMPHash(uint32_t hashSeed);
    void SetPFC(uint32_t pfc);
    void SetCC(uint32_t cc);
    
    void SetId(uint32_t id);
    uint32_t GetId();

    void SetOutput(std::string output);

    bool IngressPipeline(Ptr<Packet> packet, uint16_t protocol, Ptr<PointToPointNetDevice> dev);
    Ptr<Packet> EgressPipeline(Ptr<Packet> packet, uint16_t protocol, Ptr<PointToPointNetDevice> dev);

protected:
    std::string m_output;

    uint32_t m_nid;
    int m_hashSeed;

    std::unordered_map<uint32_t, std::vector<uint32_t>> m_route;

    // Buffer Management
    uint64_t m_drops = 0;

    static const int32_t RESERVED_SIZE = 10000; // 10KB per port
    static const int32_t RESUME_OFFSET = 10000;
    
    int32_t m_bufferTotal{0};

    int32_t m_sharedTotal{0};
    int32_t m_usedShared{0};

    int32_t m_reservedTotal{0};
    int32_t m_hdrmTotal{0};

    std::unordered_map<Ptr<PointToPointNetDevice>, int32_t> m_hdrmBuffer;
    
    std::unordered_map<Ptr<PointToPointNetDevice>, int32_t> m_usedHdrm;
    std::unordered_map<Ptr<PointToPointNetDevice>, int32_t> m_usedIngress;
    std::unordered_map<Ptr<PointToPointNetDevice>, int32_t> m_usedEgress;

    int32_t GetSharedThreshold(Ptr<PointToPointNetDevice> dev);
    int32_t GetUsedShared(Ptr<PointToPointNetDevice> dev);

    // ECN setting
    uint64_t m_ecnCount = 0;
    UniformRandomVariable m_uniformVar;

    std::unordered_map<Ptr<PointToPointNetDevice>, int32_t> m_kmin;
    std::unordered_map<Ptr<PointToPointNetDevice>, int32_t> m_kmax;

    bool ShouldECN(Ptr<PointToPointNetDevice> dev);

    // PFC Management
    uint32_t m_cc{0};
    uint32_t m_pfc{0};
    std::unordered_map<Ptr<PointToPointNetDevice>, bool> m_pause;
    std::unordered_map<Ptr<PointToPointNetDevice>, uint8_t> m_bubbleRate;
    std::unordered_map<Ptr<PointToPointNetDevice>, int64_t> m_bubbleTime;
    std::unordered_map<Ptr<PointToPointNetDevice>, int64_t> m_prevBuffer;

    void SendPFC(Ptr<NetDevice> dev, bool pause);
    bool ShouldPause(Ptr<PointToPointNetDevice> dev);
    bool ShouldResume(Ptr<PointToPointNetDevice> dev);

    void CheckBubble(Ptr<PointToPointNetDevice> dev);
};

} // namespace ns3

#endif /* SWITCH_NODE_H */
