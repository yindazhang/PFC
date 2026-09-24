#include "packet-tag.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("PacketTag");

NS_OBJECT_ENSURE_REGISTERED(PacketTag);

TypeId
PacketTag::GetTypeId()
{
    static TypeId tid = TypeId("PacketTag").SetParent<Tag>().AddConstructor<PacketTag>();
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
    return 12;
}

void
PacketTag::Serialize(TagBuffer i) const
{
    i.WriteU32(m_size);
    // The simulation is single-process: the ingress device travels as a pointer
    i.WriteU64(reinterpret_cast<uint64_t>(GetPointer(m_device)));
}

void
PacketTag::Deserialize(TagBuffer i)
{
    m_size = i.ReadU32();
    m_device = reinterpret_cast<PointToPointNetDevice*>(i.ReadU64());
}

void
PacketTag::Print(std::ostream& os) const
{
    os << "size=" << m_size;
}

void
PacketTag::SetSize(uint32_t size)
{
    m_size = size;
}

uint32_t
PacketTag::GetSize() const
{
    return m_size;
}

void
PacketTag::SetNetDevice(Ptr<PointToPointNetDevice> device)
{
    m_device = device;
}

Ptr<PointToPointNetDevice>
PacketTag::GetNetDevice() const
{
    return m_device;
}

} // namespace ns3
