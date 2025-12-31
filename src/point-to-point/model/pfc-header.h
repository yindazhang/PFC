#ifndef PFC_HEADER_H
#define PFC_HEADER_H

#include "ns3/header.h"

namespace ns3
{

class PfcHeader : public Header
{
public:
    PfcHeader();
	~PfcHeader() override;

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize() const override;

    void SetTime(uint32_t time);
    void SetQueueSize(uint32_t size);
    void SetQueueIndex(uint32_t index);

    uint32_t GetTime() const;
    uint32_t GetQueueSize() const;
    uint32_t GetQueueIndex() const;

private:
    uint32_t m_time;
    uint32_t m_queueSize;
    uint32_t m_queueIndex;
};

} // namespace ns3

#endif /* PFC_HEADER_H */
