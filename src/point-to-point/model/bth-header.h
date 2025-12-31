#ifndef BTH_HEADER_H
#define BTH_HEADER_H

#include "ns3/header.h"

namespace ns3
{

class BthHeader : public Header
{
public:
    BthHeader();
	~BthHeader() override;


    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize() const override;

    uint8_t GetOpcode();
    void SetOpcode(uint8_t opcode);

    uint8_t GetCNP();
    void SetCNP();

    uint8_t GetACK();
    void SetACK();

    uint8_t GetNACK();
    void SetNACK();

    uint16_t GetSize();
    void SetSize(uint16_t size);

    uint32_t GetId();
    void SetId(uint32_t id);

    uint32_t GetSequence();
    void SetSequence(uint32_t sequence);

    static const uint16_t ROCE_UDP_PORT = 4791;

private:
    uint8_t m_opcode;
    uint8_t m_flags;
    uint16_t m_size;
    uint32_t m_id;
    uint32_t m_sequence;
};

} // namespace ns3

#endif /* BTH_HEADER_H */
