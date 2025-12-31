#ifndef HPCC_HEADER_H
#define HPCC_HEADER_H

#include "ns3/header.h"
#include "ns3/uinteger.h"
#include "ns3/data-rate.h"

namespace ns3
{

class IntHeader : public Header
{
public:
    IntHeader();
	~IntHeader() override;

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize() const override;

    void Set(DataRate rate, uint64_t bytes, uint64_t queueLen);

    void SetRate(DataRate rate);
    DataRate GetRate() const;

    void SetTime();
    uint64_t GetTime() const;

    void SetBytes(uint64_t bytes);
    uint64_t GetBytes() const;

    void SetQueueLen(uint64_t queueLen);
    uint64_t GetQueueLen() const;

    uint64_t GetBytesDelta(const IntHeader& old) const;
    uint64_t GetTimeDelta(const IntHeader& old) const;

private:
    union{
        struct{
            uint64_t m_rate : 4,
                    m_time : 24,
                    m_bytes : 20,
                    m_queueLen : 16;
        } m_fields;
        uint32_t m_data[2];
    };
};


class HpccHeader : public Header
{
public:
    HpccHeader();
	~HpccHeader() override;

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize() const override;

    void PushIntHeader(DataRate rate, uint64_t bytes, uint64_t queueLen);

    std::vector<IntHeader> GetIntHeaders() const;

    bool CanAddIntHeader() const;

    void StopAddIntHeader();

private:
    int8_t m_hops;
    std::vector<IntHeader> m_intHeaders;
};

} // namespace ns3

#endif /* HPCC_HEADER_H */
