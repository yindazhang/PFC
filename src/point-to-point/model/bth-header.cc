#include "bth-header.h"

#include "ns3/abort.h"
#include "ns3/assert.h"
#include "ns3/header.h"
#include "ns3/log.h"

#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("BthHeader");

NS_OBJECT_ENSURE_REGISTERED(BthHeader);

BthHeader::BthHeader()
{
    m_opcode = RC_SEND_ONLY;
    m_byte1 = 0;
    m_pkey = DEFAULT_PKEY;
    m_byte4 = 0;
    m_destQp = 0;
    m_byte8 = 0;
    m_psn = 0;
}

BthHeader::~BthHeader()
{
}

TypeId
BthHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::BthHeader")
                            .SetParent<Header>()
                            .SetGroupName("PointToPoint")
                            .AddConstructor<BthHeader>();
    return tid;
}

TypeId
BthHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
BthHeader::Print(std::ostream& os) const
{
    os << "BTH opcode=0x" << std::hex << +m_opcode << " pkey=0x" << m_pkey << std::dec
       << " dqp=" << m_destQp << " psn=" << m_psn << " pad=" << +GetPadCnt()
       << " tver=" << +GetTVer();
    if (GetSolicitedEvent())
        os << " SE";
    if (GetMigReq())
        os << " M";
    if (GetFECN())
        os << " FECN";
    if (GetBECN())
        os << " BECN";
    if (GetAckReq())
        os << " A";
    if (GetRTX())
        os << " RTX";
    if (GetTSH())
        os << " TSH";
    if (GetTrimmed())
        os << " TRIM";

    if (m_opcode == RC_ACKNOWLEDGE)
    {
        os << " AETH[syndrome=0x" << std::hex << +m_aethSyndrome << std::dec
           << " msn=" << m_aethMsn << "]";
    }
    else if (IsMrcRequest())
    {
        os << " METH[msn=" << m_meth.msn << " npkts=" << m_meth.numPackets << "]";
        if (GetTSH())
        {
            os << " TSETH[ts=" << m_tseth.timestamp << "]";
        }
        os << " RETH[va=0x" << std::hex << m_reth.virtualAddress << " rkey=0x" << m_reth.rkey
           << std::dec << " len=" << m_reth.dmaLength << "]";
        if (IsMrcImm())
        {
            os << " ImmDt[0x" << std::hex << m_immData << std::dec << "]";
        }
    }
    else if (m_opcode == MRC_SACK)
    {
        os << " SETH[cack=" << m_seth.cumulativeAck << " base=" << m_seth.sackBase
           << " bitmap=0x" << std::hex << m_seth.sackBitmap << std::dec
           << " ev=" << m_seth.reflectedEv << " mpr=" << m_seth.mpr << " flags=0x" << std::hex
           << +m_seth.flags << std::dec << "]"
           << " CC_STATE[rcvd=" << m_ccState.rcvdBytes << " ts=" << m_ccState.echoTimestamp
           << " svc=" << m_ccState.serviceTime << " ecn=" << m_ccState.ecnMarkedPackets
           << " bp=" << +m_ccState.hostBackpressure << " lb=" << +m_ccState.lbMarkedPackets << "]";
    }
    else if (m_opcode == MRC_NACK)
    {
        os << " NETH[psn=" << m_neth.psn << " code=" << +m_neth.code << "]";
    }
    else if (m_opcode == MRC_PROBE)
    {
        os << " PETH[probe_id=" << m_peth.probeId << "]";
    }
    else if (m_opcode == MRC_EP_REQUEST)
    {
        os << " ERTH[op=" << +m_erth.op << " ev=" << m_erth.ev << " port_status=0x" << std::hex
           << m_erth.portStatusMask << std::dec << "]";
    }
    else if (m_opcode == MRC_EP_RESPONSE)
    {
        os << " EETH[op=" << +m_eeth.op << " status=" << +m_eeth.status << " ev=" << m_eeth.ev
           << " port_status=0x" << std::hex << m_eeth.portStatusMask << std::dec << "]";
    }
}

uint32_t
BthHeader::GetSerializedSize() const
{
    uint32_t size = BASE_SIZE;

    if (m_opcode == RC_ACKNOWLEDGE)
    {
        size += AETH_SIZE;
    }
    else if (m_opcode == ROCE_CNP)
    {
        size += 16; // reserved
    }
    else if (IsMrcRequest())
    {
        size += MrcMeth::SIZE + MrcReth::SIZE;
        if (GetTSH())
        {
            size += MrcTseth::SIZE;
        }
        if (IsMrcImm())
        {
            size += 4;
        }
    }
    else if (m_opcode == MRC_SACK)
    {
        size += MrcSeth::SIZE + MrcCcState::SIZE;
    }
    else if (m_opcode == MRC_NACK)
    {
        size += MrcNeth::SIZE;
    }
    else if (m_opcode == MRC_PROBE)
    {
        size += MrcPeth::SIZE;
    }
    else if (m_opcode == MRC_EP_REQUEST)
    {
        size += MrcErth::SIZE;
    }
    else if (m_opcode == MRC_EP_RESPONSE)
    {
        size += MrcEeth::SIZE;
    }

    return size;
}

void
BthHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteU8(m_opcode);
    start.WriteU8(m_byte1);
    start.WriteHtonU16(m_pkey);
    start.WriteHtonU32((uint32_t(m_byte4) << 24) | (m_destQp & QPN_MASK));
    start.WriteHtonU32((uint32_t(m_byte8) << 24) | (m_psn & PSN_MASK));

    if (m_opcode == RC_ACKNOWLEDGE)
    {
        start.WriteHtonU32((uint32_t(m_aethSyndrome) << 24) | (m_aethMsn & 0xFFFFFF));
    }
    else if (m_opcode == ROCE_CNP)
    {
        start.WriteU8(0, 16);
    }
    else if (IsMrcRequest())
    {
        start.WriteHtonU32(m_meth.msn);
        start.WriteHtonU16(m_meth.numPackets);
        start.WriteHtonU16(m_meth.reserved);

        if (GetTSH())
        {
            start.WriteHtonU64(m_tseth.timestamp);
        }

        start.WriteHtonU64(m_reth.virtualAddress);
        start.WriteHtonU32(m_reth.rkey);
        start.WriteHtonU32(m_reth.dmaLength);

        if (IsMrcImm())
        {
            start.WriteHtonU32(m_immData);
        }
    }
    else if (m_opcode == MRC_SACK)
    {
        start.WriteHtonU32(m_seth.cumulativeAck);
        start.WriteHtonU32(m_seth.sackBase);
        start.WriteHtonU64(m_seth.sackBitmap);
        start.WriteHtonU32(m_seth.reflectedEv);
        start.WriteHtonU16(m_seth.mpr);
        start.WriteU8(m_seth.flags);
        start.WriteU8(m_seth.reserved);

        start.WriteHtonU64(m_ccState.rcvdBytes);
        start.WriteHtonU64(m_ccState.echoTimestamp);
        start.WriteHtonU32(m_ccState.serviceTime);
        start.WriteHtonU16(m_ccState.ecnMarkedPackets);
        start.WriteU8(m_ccState.hostBackpressure);
        start.WriteU8(m_ccState.lbMarkedPackets);
    }
    else if (m_opcode == MRC_NACK)
    {
        start.WriteHtonU32(m_neth.psn);
        start.WriteU8(m_neth.code);
        start.Write(m_neth.reserved, sizeof(m_neth.reserved));
    }
    else if (m_opcode == MRC_PROBE)
    {
        start.WriteHtonU32(m_peth.probeId);
        start.WriteHtonU32(m_peth.reserved);
    }
    else if (m_opcode == MRC_EP_REQUEST)
    {
        start.WriteU8(m_erth.op);
        start.Write(m_erth.reserved, sizeof(m_erth.reserved));
        start.WriteHtonU32(m_erth.ev);
        start.WriteHtonU32(m_erth.portStatusMask);
    }
    else if (m_opcode == MRC_EP_RESPONSE)
    {
        start.WriteU8(m_eeth.op);
        start.WriteU8(m_eeth.status);
        start.Write(m_eeth.reserved, sizeof(m_eeth.reserved));
        start.WriteHtonU32(m_eeth.ev);
        start.WriteHtonU32(m_eeth.portStatusMask);
    }
}

uint32_t
BthHeader::Deserialize(Buffer::Iterator start)
{
    m_opcode = start.ReadU8();
    m_byte1 = start.ReadU8();
    m_pkey = start.ReadNtohU16();
    uint32_t word = start.ReadNtohU32();
    m_byte4 = word >> 24;
    m_destQp = word & QPN_MASK;
    word = start.ReadNtohU32();
    m_byte8 = word >> 24;
    m_psn = word & PSN_MASK;

    if (m_opcode == RC_ACKNOWLEDGE)
    {
        word = start.ReadNtohU32();
        m_aethSyndrome = word >> 24;
        m_aethMsn = word & 0xFFFFFF;
    }
    else if (m_opcode == ROCE_CNP)
    {
        start.Next(16);
    }
    else if (IsMrcRequest())
    {
        m_meth.msn = start.ReadNtohU32();
        m_meth.numPackets = start.ReadNtohU16();
        m_meth.reserved = start.ReadNtohU16();

        if (GetTSH())
        {
            m_tseth.timestamp = start.ReadNtohU64();
        }

        m_reth.virtualAddress = start.ReadNtohU64();
        m_reth.rkey = start.ReadNtohU32();
        m_reth.dmaLength = start.ReadNtohU32();

        if (IsMrcImm())
        {
            m_immData = start.ReadNtohU32();
        }
    }
    else if (m_opcode == MRC_SACK)
    {
        m_seth.cumulativeAck = start.ReadNtohU32();
        m_seth.sackBase = start.ReadNtohU32();
        m_seth.sackBitmap = start.ReadNtohU64();
        m_seth.reflectedEv = start.ReadNtohU32();
        m_seth.mpr = start.ReadNtohU16();
        m_seth.flags = start.ReadU8();
        m_seth.reserved = start.ReadU8();

        m_ccState.rcvdBytes = start.ReadNtohU64();
        m_ccState.echoTimestamp = start.ReadNtohU64();
        m_ccState.serviceTime = start.ReadNtohU32();
        m_ccState.ecnMarkedPackets = start.ReadNtohU16();
        m_ccState.hostBackpressure = start.ReadU8();
        m_ccState.lbMarkedPackets = start.ReadU8();
    }
    else if (m_opcode == MRC_NACK)
    {
        m_neth.psn = start.ReadNtohU32();
        m_neth.code = start.ReadU8();
        start.Read(m_neth.reserved, sizeof(m_neth.reserved));
    }
    else if (m_opcode == MRC_PROBE)
    {
        m_peth.probeId = start.ReadNtohU32();
        m_peth.reserved = start.ReadNtohU32();
    }
    else if (m_opcode == MRC_EP_REQUEST)
    {
        m_erth.op = start.ReadU8();
        start.Read(m_erth.reserved, sizeof(m_erth.reserved));
        m_erth.ev = start.ReadNtohU32();
        m_erth.portStatusMask = start.ReadNtohU32();
    }
    else if (m_opcode == MRC_EP_RESPONSE)
    {
        m_eeth.op = start.ReadU8();
        m_eeth.status = start.ReadU8();
        start.Read(m_eeth.reserved, sizeof(m_eeth.reserved));
        m_eeth.ev = start.ReadNtohU32();
        m_eeth.portStatusMask = start.ReadNtohU32();
    }

    return GetSerializedSize();
}

void
BthHeader::SetOpcode(uint8_t opcode)
{
    m_opcode = opcode;
}

uint8_t
BthHeader::GetOpcode() const
{
    return m_opcode;
}

bool
BthHeader::GetSolicitedEvent() const
{
    return m_byte1 & B1_SE;
}

void
BthHeader::SetSolicitedEvent(bool value)
{
    SetBit(m_byte1, B1_SE, value);
}

bool
BthHeader::GetMigReq() const
{
    return m_byte1 & B1_MIG;
}

void
BthHeader::SetMigReq(bool value)
{
    SetBit(m_byte1, B1_MIG, value);
}

uint8_t
BthHeader::GetPadCnt() const
{
    return (m_byte1 & B1_PAD_MASK) >> B1_PAD_SHIFT;
}

void
BthHeader::SetPadCnt(uint8_t padCnt)
{
    NS_ASSERT(padCnt < 4);
    m_byte1 = (m_byte1 & ~B1_PAD_MASK) | ((padCnt << B1_PAD_SHIFT) & B1_PAD_MASK);
}

uint8_t
BthHeader::GetTVer() const
{
    return m_byte1 & B1_TVER_MASK;
}

void
BthHeader::SetTVer(uint8_t tver)
{
    m_byte1 = (m_byte1 & ~B1_TVER_MASK) | (tver & B1_TVER_MASK);
}

uint16_t
BthHeader::GetPKey() const
{
    return m_pkey;
}

void
BthHeader::SetPKey(uint16_t pkey)
{
    m_pkey = pkey;
}

bool
BthHeader::GetFECN() const
{
    return m_byte4 & B4_FECN;
}

void
BthHeader::SetFECN(bool value)
{
    SetBit(m_byte4, B4_FECN, value);
}

bool
BthHeader::GetBECN() const
{
    return m_byte4 & B4_BECN;
}

void
BthHeader::SetBECN(bool value)
{
    SetBit(m_byte4, B4_BECN, value);
}

uint32_t
BthHeader::GetDestQp() const
{
    return m_destQp;
}

void
BthHeader::SetDestQp(uint32_t qpn)
{
    NS_ASSERT_MSG(qpn <= QPN_MASK, "DestQP exceeds 24 bits: " << qpn);
    m_destQp = qpn & QPN_MASK;
}

bool
BthHeader::GetAckReq() const
{
    return m_byte8 & B8_ACKREQ;
}

void
BthHeader::SetAckReq(bool value)
{
    SetBit(m_byte8, B8_ACKREQ, value);
}

uint32_t
BthHeader::GetPsn() const
{
    return m_psn;
}

void
BthHeader::SetPsn(uint32_t psn)
{
    m_psn = psn & PSN_MASK;
}

bool
BthHeader::GetCNP() const
{
    return GetBECN() || m_opcode == ROCE_CNP;
}

void
BthHeader::SetCNP(bool value)
{
    SetBECN(value);
}

bool
BthHeader::GetACK() const
{
    return (m_opcode == RC_ACKNOWLEDGE && (m_aethSyndrome & AETH_TYPE_MASK) == AETH_TYPE_ACK) ||
           m_opcode == MRC_SACK;
}

void
BthHeader::SetACK()
{
    m_opcode = RC_ACKNOWLEDGE;
    m_aethSyndrome = AETH_TYPE_ACK | AETH_ACK_NO_CREDIT;
    SetAckReq(false);
}

bool
BthHeader::GetNACK() const
{
    return (m_opcode == RC_ACKNOWLEDGE && (m_aethSyndrome & AETH_TYPE_MASK) == AETH_TYPE_NAK) ||
           m_opcode == MRC_NACK;
}

void
BthHeader::SetNACK(uint8_t nakCode)
{
    m_opcode = RC_ACKNOWLEDGE;
    m_aethSyndrome = AETH_TYPE_NAK | (nakCode & 0x1F);
    SetAckReq(false);
}

uint8_t
BthHeader::GetAethSyndrome() const
{
    return m_aethSyndrome;
}

void
BthHeader::SetAethSyndrome(uint8_t syndrome)
{
    m_aethSyndrome = syndrome;
}

uint32_t
BthHeader::GetAethMsn() const
{
    return m_aethMsn;
}

void
BthHeader::SetAethMsn(uint32_t msn)
{
    m_aethMsn = msn & 0xFFFFFF;
}

bool
BthHeader::GetRTX() const
{
    return m_byte8 & B8_RTX;
}

void
BthHeader::SetRTX(bool value)
{
    SetBit(m_byte8, B8_RTX, value);
}

bool
BthHeader::GetTSH() const
{
    return m_byte8 & B8_TSH;
}

bool
BthHeader::GetTrimmed() const
{
    return m_byte4 & B4_TRIMMED;
}

void
BthHeader::SetTrimmed(bool value)
{
    SetBit(m_byte4, B4_TRIMMED, value);
}

bool
BthHeader::IsMrc() const
{
    return (m_opcode & MRC_OPCODE_PREFIX_MASK) == MRC_OPCODE_PREFIX;
}

bool
BthHeader::IsMrcRequest() const
{
    return IsMrc() && m_opcode <= MRC_WRITE_ONLY_WITH_IMM;
}

bool
BthHeader::IsMrcImm() const
{
    return m_opcode == MRC_WRITE_LAST_WITH_IMM || m_opcode == MRC_WRITE_ONLY_WITH_IMM;
}

void
BthHeader::SetTseth(const MrcTseth& tseth)
{
    m_tseth = tseth;
    SetBit(m_byte8, B8_TSH, true);
}

void
BthHeader::ClearTseth()
{
    m_tseth = MrcTseth();
    SetBit(m_byte8, B8_TSH, false);
}


} // namespace ns3
