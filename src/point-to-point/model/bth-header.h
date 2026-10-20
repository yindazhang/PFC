#ifndef BTH_HEADER_H
#define BTH_HEADER_H

#include "ns3/header.h"

namespace ns3
{

/*
 * MRC (Multipath Reliable Connection, OCP MRC 1.0) extension headers.
 *
 * Header stacks (per "The MRC Transport", arXiv:2606.18170, Sec. III):
 *   Request (WRITE / WRITE_IMM): BTH -> METH -> [TSETH] -> RETH -> [ImmDt]
 *   SACK:                        BTH -> SETH -> CC_STATE
 *   NACK:                        BTH -> NETH
 *   Reliability probe:           BTH -> PETH
 *   Endpoint request / response: BTH -> ERTH / EETH   (dest QP = MRC_EP_QPN)
 *
 * RETH is present in every request packet (out-of-order direct placement);
 * METH is always present but only meaningful for WRITE_IMM; TSETH presence is
 * signalled by the BTH tsh bit; ImmDt follows RETH for *_WITH_IMM opcodes.
 *
 * NOTE: the public MRC material does not publish bit-exact layouts. The field
 * sets below follow the documented semantics; widths/offsets and opcode values
 * are simulator choices, not the normative wire encoding.
 */

/** METH: Message Extended Transport Header, tracks WRITE_IMM messages. */
struct MrcMeth
{
    static const uint32_t SIZE = 8;
    uint32_t msn{0};         //!< Message sequence number of the WRITE_IMM
    uint16_t numPackets{0};  //!< Total packets in this message
    uint16_t reserved{0};
};

/** TSETH: Timestamp Extended Transport Header (requester tx timestamp). */
struct MrcTseth
{
    static const uint32_t SIZE = 8;
    uint64_t timestamp{0};   //!< Requester transmit time (ns)
};

/** RETH: RDMA Extended Transport Header, carried by every MRC request. */
struct MrcReth
{
    static const uint32_t SIZE = 16;
    uint64_t virtualAddress{0};
    uint32_t rkey{0};
    uint32_t dmaLength{0};
};

/** SETH: SACK Extended Transport Header. */
struct MrcSeth
{
    static const uint32_t SIZE = 24;
    static const uint8_t FLAG_PROBE_RESPONSE = 0x01; //!< SACK answers a PETH probe
    static const uint8_t FLAG_MPR_VALID = 0x02;      //!< mpr carries a Dynamic MPR update
    static const uint8_t FLAG_DUPLICATE = 0x04;      //!< Triggered by an already-received PSN

    uint32_t cumulativeAck{0}; //!< All PSNs below this have been received
    uint32_t sackBase{0};      //!< PSN of bit 0 in sackBitmap
    uint64_t sackBitmap{0};    //!< Out-of-order arrivals relative to sackBase
    uint32_t reflectedEv{0};   //!< Entropy value of the packet that triggered the SACK
    uint16_t mpr{0};           //!< Maximum PSN Range (units of 128 PSNs)
    uint8_t flags{0};
    uint8_t reserved{0};
};

/** CC_STATE: congestion-control telemetry appended to every SACK. */
struct MrcCcState
{
    static const uint32_t SIZE = 24;
    uint64_t rcvdBytes{0};         //!< Cumulative bytes received by the responder
    uint64_t echoTimestamp{0};     //!< TSETH timestamp reflected back
    uint32_t serviceTime{0};       //!< Responder processing time (ns) to subtract from RTT
    uint16_t ecnMarkedPackets{0};  //!< Forward-path CE marks seen since the last SACK
    uint8_t hostBackpressure{0};   //!< Responder host-side congestion level / cwnd penalty
    uint8_t reserved{0};
};

/** NETH: NACK Extended Transport Header. */
struct MrcNeth
{
    static const uint32_t SIZE = 8;

    enum NackCode : uint8_t
    {
        NACK_TRIMMED = 1,            //!< Trimmed (header-only) packet received
        NACK_RESOURCE_EXHAUSTED = 2, //!< Responder local resource exhaustion
        NACK_OUT_OF_MPR = 3,         //!< PSN outside the advertised MPR window
        NACK_WIMM_LIMIT = 4,         //!< Max in-flight WRITE_IMM exceeded
        NACK_REMOTE_ACCESS = 5,      //!< RETH/rkey validation failure
    };

    uint32_t psn{0};    //!< PSN being negatively acknowledged
    uint8_t code{0};    //!< NackCode
    uint8_t reserved[3]{0, 0, 0};
};

/** PETH: reliability Probe Extended Transport Header (answered with a SACK). */
struct MrcPeth
{
    static const uint32_t SIZE = 8;
    uint32_t probeId{0};
    uint32_t reserved{0};
};

/** Endpoint operation types carried in ERTH/EETH. */
enum MrcEpOp : uint8_t
{
    MRC_EP_EV_PROBE = 1,
    MRC_EP_PORT_STATUS_UPDATE = 2,
};

/** ERTH: Endpoint Request Transport Header. */
struct MrcErth
{
    static const uint32_t SIZE = 12;
    uint8_t op{0};                 //!< MrcEpOp
    uint8_t reserved[3]{0, 0, 0};
    uint32_t ev{0};                //!< EV under test (EV probe)
    uint32_t portStatusMask{0};    //!< Local port reachability bitmap (port status update)
};

/** EETH: Endpoint Response Transport Header. */
struct MrcEeth
{
    static const uint32_t SIZE = 12;
    uint8_t op{0};                 //!< MrcEpOp being answered
    uint8_t status{0};
    uint8_t reserved[2]{0, 0};
    uint32_t ev{0};                //!< Echoed EV
    uint32_t portStatusMask{0};    //!< Responder's local port reachability bitmap
};

/**
 * InfiniBand / RoCEv2 Base Transport Header (IBTA Vol. 1, 9.2), 12 bytes:
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |    OpCode     |S|M|Pad|  TVer |            P_Key              |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |F|B|T| Resv5   |                    DestQP                     |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |A|R|H| Resv5   |                     PSN                       |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *  S=SolicitedEvent M=MigReq Pad=PadCnt F=FECN B=BECN A=AckReq
 *  MRC additions carved from reserved bits (positions are simulator choices):
 *    R=rtx (retransmission), H=tsh (TSETH present), T=trimmed (sim marker).
 *
 * The ACK opcode is followed by an AETH (4 bytes: Syndrome, MSN[23:0]).
 * Congestion feedback on ACKs is carried in BECN.
 */
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

    uint8_t GetOpcode() const;
    void SetOpcode(uint8_t opcode);

    bool GetSolicitedEvent() const;
    void SetSolicitedEvent(bool value = true);

    bool GetMigReq() const;
    void SetMigReq(bool value = true);

    uint8_t GetPadCnt() const;
    void SetPadCnt(uint8_t padCnt);

    uint8_t GetTVer() const;
    void SetTVer(uint8_t tver);

    uint16_t GetPKey() const;
    void SetPKey(uint16_t pkey);

    bool GetFECN() const;
    void SetFECN(bool value = true);

    bool GetBECN() const;
    void SetBECN(bool value = true);

    uint32_t GetDestQp() const;
    void SetDestQp(uint32_t qpn);

    bool GetAckReq() const;
    void SetAckReq(bool value = true);

    uint32_t GetPsn() const;
    void SetPsn(uint32_t psn);
    /**
     * Full PSN of the 24-bit wire \p psn: the one closest to \p reference
     * (a 32-bit PSN within 2^23 of it, e.g. the expected or cumulative PSN).
     */
    static uint32_t UnwrapPsn(uint32_t psn, uint32_t reference)
    {
        int32_t delta = int32_t(((psn - reference) & PSN_MASK) << 8) >> 8; // sign-extend 24 bits
        return reference + delta;
    }

    /** Congestion notification: BECN on an ACK, or an explicit RoCEv2 CNP. */
    bool GetCNP() const;
    /** Sets/clears BECN; a ROCE_CNP opcode still reports CNP after clearing. */
    void SetCNP(bool value = true);

    /** RC ACKNOWLEDGE with an ACK syndrome, or an MRC SACK. */
    bool GetACK() const;
    /** Turn this header into an RC ACKNOWLEDGE with an ACK syndrome. */
    void SetACK();

    /** RC ACKNOWLEDGE with a NAK syndrome, or an MRC NACK. */
    bool GetNACK() const;
    /** Turn this header into an RC ACKNOWLEDGE with a PSN-sequence-error NAK. */
    void SetNACK(uint8_t nakCode = AETH_NAK_PSN_SEQ_ERROR);

    // AETH (present for RC_ACKNOWLEDGE)
    uint8_t GetAethSyndrome() const;
    void SetAethSyndrome(uint8_t syndrome);

    uint32_t GetAethMsn() const;
    void SetAethMsn(uint32_t msn);

    // MRC BTH bits
    bool GetRTX() const;
    void SetRTX(bool value = true);

    bool GetTSH() const;

    bool GetTrimmed() const;
    void SetTrimmed(bool value = true);

    // MRC overloads the PSN as a requester-local request_id for probes / endpoint ops
    uint32_t GetRequestId() const { return GetPsn(); }
    void SetRequestId(uint32_t id) { SetPsn(id); }

    // MRC opcode classification
    bool IsMrc() const;
    bool IsMrcRequest() const;
    bool IsMrcImm() const;

    // MRC extension headers. Which ones are serialized is determined by the
    // opcode (and the tsh bit for TSETH); setters do not change the opcode.
    const MrcMeth& GetMeth() const { return m_meth; }
    void SetMeth(const MrcMeth& meth) { m_meth = meth; }

    const MrcTseth& GetTseth() const { return m_tseth; }
    void SetTseth(const MrcTseth& tseth); //!< also sets the tsh bit
    void ClearTseth();                    //!< also clears the tsh bit

    const MrcReth& GetReth() const { return m_reth; }
    void SetReth(const MrcReth& reth) { m_reth = reth; }

    uint32_t GetImmData() const { return m_immData; }
    void SetImmData(uint32_t imm) { m_immData = imm; }

    const MrcSeth& GetSeth() const { return m_seth; }
    void SetSeth(const MrcSeth& seth) { m_seth = seth; }

    const MrcCcState& GetCcState() const { return m_ccState; }
    void SetCcState(const MrcCcState& cc) { m_ccState = cc; }

    const MrcNeth& GetNeth() const { return m_neth; }
    void SetNeth(const MrcNeth& neth) { m_neth = neth; }

    const MrcPeth& GetPeth() const { return m_peth; }
    void SetPeth(const MrcPeth& peth) { m_peth = peth; }

    const MrcErth& GetErth() const { return m_erth; }
    void SetErth(const MrcErth& erth) { m_erth = erth; }

    const MrcEeth& GetEeth() const { return m_eeth; }
    void SetEeth(const MrcEeth& eeth) { m_eeth = eeth; }

    static const uint16_t ROCE_UDP_PORT = 4791;

    static const uint16_t DEFAULT_PKEY = 0xFFFF;
    static const uint32_t QPN_MASK = 0xFFFFFF;
    static const uint32_t PSN_MASK = 0xFFFFFF;

    // IBTA RC opcodes (transport prefix 000)
    enum RcOpcode : uint8_t
    {
        RC_SEND_FIRST = 0x00,
        RC_SEND_MIDDLE = 0x01,
        RC_SEND_LAST = 0x02,
        RC_SEND_LAST_WITH_IMM = 0x03,
        RC_SEND_ONLY = 0x04,
        RC_SEND_ONLY_WITH_IMM = 0x05,
        RC_WRITE_FIRST = 0x06,
        RC_WRITE_MIDDLE = 0x07,
        RC_WRITE_LAST = 0x08,
        RC_WRITE_LAST_WITH_IMM = 0x09,
        RC_WRITE_ONLY = 0x0A,
        RC_WRITE_ONLY_WITH_IMM = 0x0B,
        RC_ACKNOWLEDGE = 0x11,
        ROCE_CNP = 0x81, //!< RoCEv2 Congestion Notification Packet
    };

    // AETH syndrome: [7:5] type, [4:0] credit count / NAK code
    static const uint8_t AETH_TYPE_MASK = 0xE0;
    static const uint8_t AETH_TYPE_ACK = 0x00;
    static const uint8_t AETH_TYPE_RNR_NAK = 0x20;
    static const uint8_t AETH_TYPE_NAK = 0x60;
    static const uint8_t AETH_ACK_NO_CREDIT = 0x1F; //!< ACK with invalid credit count
    static const uint8_t AETH_NAK_PSN_SEQ_ERROR = 0x00;
    static const uint8_t AETH_NAK_INVALID_REQUEST = 0x01;
    static const uint8_t AETH_NAK_REMOTE_ACCESS = 0x02;
    static const uint8_t AETH_NAK_REMOTE_OP = 0x03;
    static const uint32_t AETH_SIZE = 4;

    // MRC opcode space: distinct transport prefix 0101 in opcode[7:4].
    static const uint8_t MRC_OPCODE_PREFIX = 0x50;
    static const uint8_t MRC_OPCODE_PREFIX_MASK = 0xF0;

    enum MrcOpcode : uint8_t
    {
        MRC_WRITE_FIRST = MRC_OPCODE_PREFIX | 0x0,
        MRC_WRITE_MIDDLE = MRC_OPCODE_PREFIX | 0x1,
        MRC_WRITE_LAST = MRC_OPCODE_PREFIX | 0x2,
        MRC_WRITE_LAST_WITH_IMM = MRC_OPCODE_PREFIX | 0x3,
        MRC_WRITE_ONLY = MRC_OPCODE_PREFIX | 0x4,
        MRC_WRITE_ONLY_WITH_IMM = MRC_OPCODE_PREFIX | 0x5,
        MRC_SACK = MRC_OPCODE_PREFIX | 0x8,
        MRC_NACK = MRC_OPCODE_PREFIX | 0x9,
        MRC_PROBE = MRC_OPCODE_PREFIX | 0xA,
        MRC_EP_REQUEST = MRC_OPCODE_PREFIX | 0xB,
        MRC_EP_RESPONSE = MRC_OPCODE_PREFIX | 0xC,
    };

    /** Reserved destination QP identifier for MRC endpoint operations. */
    static const uint32_t MRC_EP_QPN = 0x2;

    static const uint32_t BASE_SIZE = 12;

private:
    static void SetBit(uint8_t& byte, uint8_t mask, bool value)
    {
        byte = value ? (byte | mask) : (byte & ~mask);
    }

    // Byte 1
    static const uint8_t B1_SE = 0x80;
    static const uint8_t B1_MIG = 0x40;
    static const uint8_t B1_PAD_MASK = 0x30;
    static const uint8_t B1_PAD_SHIFT = 4;
    static const uint8_t B1_TVER_MASK = 0x0F;
    // Byte 4
    static const uint8_t B4_FECN = 0x80;
    static const uint8_t B4_BECN = 0x40;
    static const uint8_t B4_TRIMMED = 0x20; //!< Payload trimmed by a switch (sim marker)
    // Byte 8
    static const uint8_t B8_ACKREQ = 0x80;
    static const uint8_t B8_RTX = 0x40;     //!< MRC: retransmitted packet
    static const uint8_t B8_TSH = 0x20;     //!< MRC: TSETH present

    uint8_t m_opcode;
    uint8_t m_byte1;   //!< SE | M | PadCnt | TVer
    uint16_t m_pkey;
    uint8_t m_byte4;   //!< FECN | BECN | reserved
    uint32_t m_destQp; //!< 24 bits
    uint8_t m_byte8;   //!< AckReq | reserved
    uint32_t m_psn;    //!< 24 bits

    uint8_t m_aethSyndrome{AETH_ACK_NO_CREDIT};
    uint32_t m_aethMsn{0};

    MrcMeth m_meth;
    MrcTseth m_tseth;
    MrcReth m_reth;
    uint32_t m_immData{0};
    MrcSeth m_seth;
    MrcCcState m_ccState;
    MrcNeth m_neth;
    MrcPeth m_peth;
    MrcErth m_erth;
    MrcEeth m_eeth;
};

} // namespace ns3

#endif /* BTH_HEADER_H */
