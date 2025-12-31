#include "hpcc-header.h"

#include "ns3/abort.h"
#include "ns3/assert.h"
#include "ns3/header.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("HpccHeader");

NS_OBJECT_ENSURE_REGISTERED(HpccHeader);

IntHeader::IntHeader()
{
    m_data[0] = 0;
    m_data[1] = 0;
}

IntHeader::~IntHeader()
{
}

TypeId
IntHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::IntHeader")
                            .SetParent<Header>()        
                            .SetGroupName("PointToPoint")
                            .AddConstructor<IntHeader>();
    return tid;
}

TypeId
IntHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
IntHeader::Print(std::ostream& os) const
{
    return;
}

uint32_t
IntHeader::GetSerializedSize() const
{
    return 8; // 8 bytes
}

void
IntHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteHtonU32(m_data[0]);
    start.WriteHtonU32(m_data[1]);
}

uint32_t
IntHeader::Deserialize(Buffer::Iterator start)
{
    m_data[0] = start.ReadNtohU32();
    m_data[1] = start.ReadNtohU32();
    return GetSerializedSize();
}

void
IntHeader::Set(DataRate rate, uint64_t bytes, uint64_t queueLen)
{
    SetRate(rate);
    SetTime();
    SetBytes(bytes);
    SetQueueLen(queueLen);
}

void
IntHeader::SetRate(DataRate rate)
{
    m_fields.m_rate = rate.GetBitRate() / 1e11; // in 100Gbps units
}

DataRate
IntHeader::GetRate() const
{
    return DataRate(m_fields.m_rate * 1e11);
}

void
IntHeader::SetTime()
{
    m_fields.m_time = Simulator::Now().GetNanoSeconds() / 16; // in 16ns units
}

uint64_t
IntHeader::GetTime() const
{
    return m_fields.m_time * 16;
}

void
IntHeader::SetBytes(uint64_t bytes)
{
    m_fields.m_bytes = bytes / 512; // in 512B units
}

uint64_t
IntHeader::GetBytes() const
{
    return m_fields.m_bytes * 512;
}

void
IntHeader::SetQueueLen(uint64_t queueLen)
{
    m_fields.m_queueLen = queueLen / 64; // in 64B units
}

uint64_t
IntHeader::GetQueueLen() const
{
    return m_fields.m_queueLen * 64;
}

uint64_t
IntHeader::GetBytesDelta(const IntHeader& old) const
{
    if(GetBytes() < old.GetBytes())
    {
        uint64_t maxBytes = (1ULL << 20) * 512; // 20 bits for bytes in 512B units
        if(GetBytes() + maxBytes < old.GetBytes())
        {
            std::cerr << "IntHeader::GetBytesDelta: byte count wrap-around too large!" << std::endl;
        }
        return GetBytes() + maxBytes - old.GetBytes();
    }
    return GetBytes() - old.GetBytes();
}

uint64_t
IntHeader::GetTimeDelta(const IntHeader& old) const
{
    if(GetTime() < old.GetTime())
    {
        uint64_t maxTime = (1ULL << 24) * 16; // 24 bits for time in 16ns units
        if(GetTime() + maxTime < old.GetTime())
        {
            std::cerr << "IntHeader::GetTimeDelta: time count wrap-around too large!" << std::endl;
        }
        return GetTime() + maxTime - old.GetTime();
    }
    return GetTime() - old.GetTime();
}


HpccHeader::HpccHeader()
{
    m_hops = 0;
}

HpccHeader::~HpccHeader()
{
}

TypeId
HpccHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HpccHeader")
                            .SetParent<Header>()
                            .SetGroupName("PointToPoint")
                            .AddConstructor<HpccHeader>();
    return tid;
}

TypeId
HpccHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
HpccHeader::Print(std::ostream& os) const
{
    return;
}

uint32_t
HpccHeader::GetSerializedSize() const
{
    uint32_t size = 1; // for hops
    for (const auto& intHeader : m_intHeaders)
    {
        size += intHeader.GetSerializedSize();
    }
    return size;
}

void
HpccHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteU8(m_hops);
    for (const auto& intHeader : m_intHeaders)
    {
        intHeader.Serialize(start);
        start.Next(intHeader.GetSerializedSize());
    }
}

uint32_t
HpccHeader::Deserialize(Buffer::Iterator start)
{
    m_hops = start.ReadU8();
    for(uint8_t i = 0; i < abs(m_hops); ++i)
    {
        IntHeader intHeader;
        intHeader.Deserialize(start);
        m_intHeaders.push_back(intHeader);
        start.Next(intHeader.GetSerializedSize());
    }
    return GetSerializedSize();
}

void
HpccHeader::PushIntHeader(DataRate rate, uint64_t bytes, uint64_t queueLen)
{
    if(!CanAddIntHeader())
    {
        std::cerr << "HpccHeader::PushIntHeader: cannot add more INT headers!" << std::endl;
        return;
    }
    IntHeader intHeader;
    intHeader.Set(rate, bytes, queueLen);
    m_intHeaders.push_back(intHeader);
    m_hops += 1;
}

std::vector<IntHeader>
HpccHeader::GetIntHeaders() const
{
    return m_intHeaders;
}

bool
HpccHeader::CanAddIntHeader() const
{
    return m_hops >= 0;
}

void
HpccHeader::StopAddIntHeader()
{
    m_hops = 0 - m_hops;
}

} // namespace ns3
