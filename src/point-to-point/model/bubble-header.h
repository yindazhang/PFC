#ifndef BUBBLE_HEADER_H
#define BUBBLE_HEADER_H

#include "ns3/header.h"

namespace ns3
{

class BubbleHeader : public Header
{
public:
    BubbleHeader();
	~BubbleHeader() override;

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize() const override;

    void SetBubbleRate(uint8_t rate);
    uint8_t GetBubbleRate() const;

private:
    uint8_t m_bubbleRate;
};

} // namespace ns3

#endif /* BUBBLE_HEADER_H */