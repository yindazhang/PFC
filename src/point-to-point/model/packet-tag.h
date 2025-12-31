#ifndef PACKET_TAG_H
#define PACKET_TAG_H

#include "ns3/tag.h"
#include "ns3/node.h"
#include "point-to-point-net-device.h"

namespace ns3
{

class PacketTag : public Tag
{
  public:
    /**
     * \brief Get the type ID.
     * \return The object TypeId.
     */
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer i) const override;
    void Deserialize(TagBuffer i) override;

    void SetSize(uint32_t size);
    uint32_t GetSize();

    void SetReserve(uint32_t reserve);
    uint32_t GetReserve();

    void SetShare(uint32_t share);
    uint32_t GetShare();

    void SetHdrm(uint32_t hdrm);
    uint32_t GetHdrm();

    void SetNetDevice(Ptr<PointToPointNetDevice> device);
    Ptr<PointToPointNetDevice> GetNetDevice();

    void Print(std::ostream& os) const override;

  private:
    uint32_t m_size{0};
    uint32_t m_reserve{0};
    uint32_t m_share{0};
    uint32_t m_hdrm{0};
    Ptr<PointToPointNetDevice> m_device;
};

} // namespace ns3

#endif /* PACKET_TAG_H */
