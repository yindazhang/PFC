#ifndef PACKET_TAG_H
#define PACKET_TAG_H

#include "point-to-point-net-device.h"

#include "ns3/tag.h"

namespace ns3
{

/**
 * Switch-internal tag on low-priority packets: the bytes the packet holds in
 * the switch buffer and the ingress port they are accounted to, so the egress
 * pipeline can release them.
 */
class PacketTag : public Tag
{
  public:
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer i) const override;
    void Deserialize(TagBuffer i) override;
    void Print(std::ostream& os) const override;

    void SetSize(uint32_t size);
    uint32_t GetSize() const;

    void SetNetDevice(Ptr<PointToPointNetDevice> device);
    Ptr<PointToPointNetDevice> GetNetDevice() const;

  private:
    uint32_t m_size{0};
    Ptr<PointToPointNetDevice> m_device;
};

} // namespace ns3

#endif /* PACKET_TAG_H */
