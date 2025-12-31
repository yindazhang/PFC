#include "packet-tag.h"

#include "ns3/abort.h"
#include "ns3/assert.h"
#include "ns3/tag.h"
#include "ns3/node.h"
#include "ns3/nstime.h"
#include "ns3/log.h"

#include "point-to-point-net-device.h"

#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("PacketTag");

NS_OBJECT_ENSURE_REGISTERED(PacketTag);


TypeId
PacketTag::GetTypeId()
{
    static TypeId tid = TypeId("PacketTag")
                            .SetParent<Tag>()
                            .AddConstructor<PacketTag>();
    return tid;
}

TypeId
PacketTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
PacketTag::GetSerializedSize() const
{
    return 24;
}

void
PacketTag::Serialize(TagBuffer i) const
{
    i.WriteU32(m_size);
    i.WriteU32(m_reserve);
    i.WriteU32(m_share);
    i.WriteU32(m_hdrm);
    i.WriteU64(reinterpret_cast<uint64_t>(GetPointer(m_device)));
}

void
PacketTag::Deserialize(TagBuffer i)
{
    m_size = i.ReadU32();
    m_reserve = i.ReadU32();
    m_share = i.ReadU32();
    m_hdrm = i.ReadU32();
    m_device = (PointToPointNetDevice*)(i.ReadU64());
}

void 
PacketTag::SetSize(uint32_t size)
{
    m_size = size;
}

uint32_t 
PacketTag::GetSize()
{
    return m_size;
}

void
PacketTag::SetReserve(uint32_t reserve)
{
    m_reserve = reserve;
}

uint32_t
PacketTag::GetReserve()
{
    return m_reserve;
}

void
PacketTag::SetShare(uint32_t share)
{
    m_share = share;
}

uint32_t
PacketTag::GetShare()
{
    return m_share;
}

void
PacketTag::SetHdrm(uint32_t hdrm)
{
    m_hdrm = hdrm;
}

uint32_t
PacketTag::GetHdrm()
{
    return m_hdrm;
}

void 
PacketTag::SetNetDevice(Ptr<PointToPointNetDevice> device)
{
    m_device = device;
}

Ptr<PointToPointNetDevice> 
PacketTag::GetNetDevice()
{
    return m_device;
}

void
PacketTag::Print(std::ostream& os) const
{
    return;
}

} // namespace ns3
