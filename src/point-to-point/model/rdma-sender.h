#ifndef RDMA_SENDER_H
#define RDMA_SENDER_H

#include "bth-header.h"
#include "srv6-header.h"

#include "ns3/callback.h"
#include "ns3/data-rate.h"
#include "ns3/event-id.h"
#include "ns3/ipv6-address.h"
#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/packet.h"

#include <deque>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace ns3
{

/** A long-lived connection (QP) from host \p src to host \p dst. */
struct QpInfo
{
    uint32_t id{0}; //!< QP number (BTH DestQP) on both ends
    uint32_t src{0};
    uint32_t dst{0};
    uint64_t minRttNs{0}; //!< Base (propagation) RTT
};

/**
 * MRC requester for one long-lived QP, installed directly at the source host.
 *
 * Messages posted with PostMessage() are sent in order on one continuous PSN
 * stream (RC semantics): each is an RDMA WRITE_WITH_IMM whose last packet
 * carries the message id as immediate data, so the responder completes it
 * (RdmaReceiver message callback) once every packet of it arrived. The
 * congestion window, RTT estimates and EV set carry over from one message to
 * the next; the sender idles (no packets, no timer) while nothing is posted.
 *
 * Multi-plane: each plane (NetDevice) registers with AddPlane() and pulls
 * packets with GenerateNextPacket() whenever it can transmit. The congestion
 * window, pacing (at the sum of the plane rates) and the MPR are shared by
 * all planes.
 *
 * Every packet is IPv6 / UDP(4791) / BTH (MRC_WRITE_*) + METH [+ TSETH] + RETH
 * + payload, carries its byte offset in RETH (out-of-order placement) and an
 * entropy value (EV) in the IPv6 flow label, echoed back in SACKs.
 *
 * Path selection:
 *   - Default: hash-based ECMP. With PacketSpraying, a fresh EV per packet.
 *   - SourceRouting (MRC with static SRv6): every EV maps to one SRv6 path
 *     given with AddPath() (plane + switches). At startup the sender picks
 *     EvsPerPlane random paths per plane as the active EV set (the rest are
 *     backups) and each plane sends on its own active EVs in turn:
 *       * ECN on a SACK (or a trim NACK) for an EV: skip it for EcnSkipRtts
 *         base RTTs (ECN as a load-balancing signal);
 *       * loss that is not a trim (reorder detection, RTO): the EV is
 *         ASSUMED_BAD and replaced by a backup EV of the same plane, as long
 *         as at least EvMinActive EVs stay active. (No EV probes: an
 *         ASSUMED_BAD EV is not resurrected.)
 *
 * Reliability: selective. SACKs advance the cumulative ack and mark
 * out-of-order arrivals; NACKs (trimmed / out of MPR) and time-based reorder
 * detection (RACK-like: a packet is lost once a packet sent after it was
 * acknowledged and it has been outstanding for more than the smoothed RTT plus
 * a reorder window, which doubles on spurious detections) mark packets lost;
 * an RTO (exponential backoff)
 * marks everything in flight lost. Lost packets are retransmitted first,
 * earliest PSN first, with the BTH rtx bit set.
 *
 * Congestion control: NSCC (UEC, based on SMaRTT), window-based and SACK-
 * clocked, one window per connection across all paths. On each SACK, with
 * the ECN signal and an RTT sample:
 *   - ECN, RTT > target:      multiplicative decrease, at most once per base RTT
 *   - ECN, RTT <= target:     no change (congestion only building up)
 *   - no ECN, RTT > target:   fair increase
 *   - no ECN, RTT <= target:  proportional increase (+ fair increase)
 * plus Fast Increase after a window of uncongested bytes at base RTT, and
 * Quick Adapt: after a trim NACK or an RTO, the window is set to the bytes
 * acknowledged over the last target RTT, and congestion signals of packets
 * already in flight are ignored.
 */
class RdmaSender : public Object
{
public:
    static TypeId GetTypeId();

    explicit RdmaSender(QpInfo qp = QpInfo());
    ~RdmaSender() override;

    /** IPv6 address of host \p hostId (fd00::<hostId>), used as QP src/dst. */
    static Ipv6Address GetHostAddress(uint32_t hostId);

    uint32_t GetId() const { return m_qp.id; }

    /** Queue a message of \p size bytes with id \p msgId (the immediate data). */
    void PostMessage(uint32_t msgId, uint32_t size);

    /** Invoked with the message id once every byte of a message is acknowledged. */
    void SetMessageCallback(Callback<void, uint32_t> callback) { m_onMessageAcked = callback; }

    /**
     * SourceRouting: add an SRv6 path on \p plane (the plane index returned
     * by AddPlane(), in registration order). Add every path before AddPlane()
     * of that plane.
     */
    void AddPath(uint32_t plane, const Srv6Sid& path);

    /**
     * Register a plane of \p rate. \p wakeup is invoked when the sender gets
     * data to send again after being blocked (window, loss, timeout, idle).
     * \return the plane index to pass to GenerateNextPacket().
     */
    uint32_t AddPlane(DataRate rate, Callback<void> wakeup);

    /**
     * Next packet (starting with the IPv6 header) for \p plane, or nullptr
     * if pacing, the window, the MPR or (SourceRouting) the plane's active
     * EVs do not allow one now.
     */
    Ptr<Packet> GenerateNextPacket(uint32_t plane);

    /** Process a SACK/NACK (starting with its IPv6 header). */
    void Receive(Ptr<Packet> packet);

    /** True while \p plane may send: new or lost data, an open window, an active EV. */
    bool HasPendingData(uint32_t plane) const;
    /** Earliest time (ns) pacing allows \p plane the next packet; -1 until woken up. */
    int64_t GetNextSendTime(uint32_t plane) const;
    /** True while every posted byte is acknowledged. */
    bool IsIdle() const { return m_cumAck == m_postedPsn; }

    uint32_t GetCwnd() const { return m_cwnd; }

    /** Totals over every sender of the simulation. */
    struct GlobalStats
    {
        uint64_t messages{0};        //!< Messages acknowledged
        uint64_t packets{0};         //!< Data packets sent (incl. retransmissions)
        uint64_t retransmissions{0};
        uint64_t rtos{0};            //!< Timeouts that marked packets lost
        uint64_t reorderLosses{0};   //!< Packets declared lost by reorder detection
        uint64_t spuriousDetections{0}; //!< Reorder-window widenings (spurious losses seen)
        uint64_t trimNacks{0};
        uint64_t evSkips{0};         //!< ECN / trim signals on an EV
        uint64_t evFailures{0};      //!< EVs retired as ASSUMED_BAD
        uint64_t evRestores{0};      //!< Retired EVs brought back (the loss was spurious)
    };

    static const GlobalStats& GetGlobalStats() { return s_stats; }

protected:
    void NotifyConstructionCompleted() override;
    void DoDispose() override;

private:
    enum PacketState : uint8_t
    {
        EMPTY = 0, //!< Not sent yet (or already cumulatively acked)
        IN_FLIGHT,
        SACKED,
        LOST, //!< Waiting in m_lost for retransmission
    };

    struct PacketInfo
    {
        PacketState state{EMPTY};
        bool retransmitted{false}; //!< No RTT sample; excluded from reorder loss detection
        uint32_t payload{0};       //!< Payload bytes
        int64_t sendTime{0};       //!< ns
        int32_t ev{-1};            //!< EV used (SourceRouting), -1 otherwise
    };

    /** A posted message: PSNs [firstPsn, firstPsn + numPackets). */
    struct Message
    {
        uint32_t id;
        uint32_t size;
        uint32_t msn; //!< Message sequence number on this QP
        uint32_t firstPsn;
        uint32_t numPackets;
    };

    enum EvState : uint8_t
    {
        EV_GOOD,
        EV_ASSUMED_BAD,
    };

    struct EntropyValue
    {
        uint32_t plane;
        Srv6Sid path;
        EvState state{EV_GOOD};
        int64_t skipUntil{0}; //!< ns: skipped after an ECN / trim signal
    };

    PacketInfo& Info(uint32_t psn) { return m_window[psn % m_window.size()]; }
    /** Grow the ring (up to the MPR) until every PSN in [m_cumAck, m_nextPsn) has a slot. */
    void GrowWindow();
    /** The unacknowledged message \p psn belongs to. */
    const Message& MessageOf(uint32_t psn) const;
    bool CanSendNew() const;
    bool IsWindowOpen() const;
    bool HasDataToSend() const;

    Ptr<Packet> BuildPacket(const Message& msg, uint32_t psn, bool retransmission, int32_t ev);
    uint32_t NextEntropy();

    // Reliability
    void ProcessSack(const BthHeader& bth);
    void ProcessNack(const BthHeader& bth);
    /** Mark \p psn acknowledged; returns its payload bytes if it was not already. */
    uint32_t Acknowledge(uint32_t psn);
    void MarkLost(uint32_t psn);
    void DetectReorderLoss();
    void WidenReorderWindow();

    void RestartRto();
    void OnRto();
    void NotifyPlanes();
    /** Report every message the cumulative ack has passed. */
    void CompleteMessages();

    // EV management (SourceRouting)
    void BuildEvSet(uint32_t plane);
    int32_t SelectEv(uint32_t plane);
    void OnEvCongested(int32_t ev);
    /** Retire \p ev after the loss of \p psn (may be undone by RestoreEv). */
    void OnEvLoss(int32_t ev, uint32_t psn);
    /** A duplicate SACK proved \p psn was delivered: bring back the EV retired for it. */
    void RestoreEv(uint32_t psn);

    // NSCC
    void UpdateLimits();
    void OnAck(uint32_t ackedBytes, bool ecn, int64_t rtt);
    void QuickAdapt();
    void TriggerQuickAdapt();
    void ClampCwnd();

    // QP
    QpInfo m_qp;
    Ipv6Address m_srcAddress;
    Ipv6Address m_dstAddress;

    // Attributes
    uint32_t m_mtuShift;              //!< MTU = 1 << m_mtuShift payload bytes
    uint32_t m_rttSerializationBytes; //!< Serialization added to the base RTT
    uint32_t m_mpr;                   //!< Maximum PSN Range (outstanding packets)
    double m_reorderWindow;           //!< Initial reorder window, in base RTTs
    Time m_minRto;
    Time m_maxRto;
    bool m_packetSpraying;
    bool m_useTimestamp;
    double m_targetRttFactor; //!< Target RTT = factor * base RTT
    double m_maxCwndBdp;      //!< Window cap in BDPs
    double m_fairIncrease;    //!< BDP fraction added per window of acked bytes (fi)
    uint32_t m_fastIncrease;  //!< MTUs added per ack during Fast Increase (k)
    bool m_sourceRouting;     //!< SRv6 source routing + EV management
    uint32_t m_evsPerPlane;   //!< Active EVs per plane (0: half of its paths)
    uint32_t m_evMinActive;   //!< Minimum active EVs per plane
    double m_ecnSkipRtts;     //!< EV skip time after ECN, in base RTTs

    uint32_t m_mtu{0};

    // Planes
    DataRate m_lineRate; //!< Sum of the plane rates
    std::vector<Callback<void>> m_planeWakeups;

    // EV management (SourceRouting): EV value = index in m_evs
    std::vector<EntropyValue> m_evs;
    std::vector<std::vector<uint32_t>> m_planePaths; //!< [plane] -> EVs added by AddPath
    std::vector<std::vector<uint32_t>> m_activeEvs;  //!< [plane] -> active EVs, rotated
    std::vector<std::vector<uint32_t>> m_backupEvs;  //!< [plane] -> spare GOOD EVs
    std::vector<uint32_t> m_evCursor;                //!< [plane] -> next active EV
    std::mt19937 m_evRng;
    std::map<uint32_t, uint32_t> m_retiredFor; //!< PSN whose loss retired an EV -> that EV
    std::vector<uint32_t> m_ecmpEvs; //!< Hash-based EV set (EcmpEvSetSize > 0), rotated
    uint32_t m_ecmpEvSetSize;
    static GlobalStats s_stats;

    // Messages
    std::deque<Message> m_messages; //!< Posted and not yet acknowledged, in PSN order
    uint32_t m_postedPsn{0};         //!< End of the posted PSNs
    uint32_t m_nextMsn{0};
    Callback<void, uint32_t> m_onMessageAcked;

    // Reliability
    uint32_t m_nextPsn{0}; //!< Next never-sent PSN
    uint32_t m_cumAck{0};  //!< Every PSN below it is acknowledged
    std::vector<PacketInfo> m_window; //!< Ring over [m_cumAck, m_nextPsn), grown up to the MPR
    std::set<uint32_t> m_lost;        //!< PSNs to retransmit, earliest first
    uint64_t m_inflightBytes{0};
    int64_t m_rackXmitTime{-1}; //!< Latest send time of an acknowledged packet (ns)
    double m_reorderWindowRtts{1.0}; //!< Current reorder window, in base RTTs

    Time m_rto;
    EventId m_rtoEvent;

    uint32_t m_entropy{0};
    uint64_t m_packetCount{0};
    int64_t m_lastSendTime{-1}; //!< For pacing at m_lineRate

    // NSCC state
    int64_t m_baseRtt{0};   //!< ns: configured estimate, then the minimum RTT sample
    int64_t m_minRttSample{INT64_MAX};
    int64_t m_targetRtt{0}; //!< ns
    double m_avgRtt{0};     //!< ns, EWMA
    uint32_t m_bdp{0};      //!< line rate * base RTT, bytes
    uint32_t m_maxCwnd{0};
    uint32_t m_cwnd{0};
    int64_t m_lastDecreaseTime{-1};

    uint32_t m_fastIncreaseCount{0}; //!< Uncongested bytes in a row
    bool m_fastIncreaseActive{false};

    uint64_t m_qaAcked{0}; //!< Bytes acked in the current Quick Adapt period
    int64_t m_qaEnd{0};    //!< End of the current Quick Adapt period (ns)
    bool m_qaTriggered{false};
    uint64_t m_bytesToIgnore{0};
    uint64_t m_bytesIgnored{0};
};

} // namespace ns3

#endif /* RDMA_SENDER_H */
