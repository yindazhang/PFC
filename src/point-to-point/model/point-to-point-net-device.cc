/*
 * Copyright (c) 2007, 2008 University of Washington
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "point-to-point-net-device.h"

#include "point-to-point-channel.h"

#include "switch-node.h"
#include "ppp-header.h"
#include "pfc-header.h"

#include "ns3/error-model.h"
#include "ns3/llc-snap-header.h"
#include "ns3/log.h"
#include "ns3/mac48-address.h"
#include "ns3/pointer.h"
#include "ns3/queue.h"
#include "ns3/simulator.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/uinteger.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"

#include <unordered_set>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("PointToPointNetDevice");

NS_OBJECT_ENSURE_REGISTERED(PointToPointNetDevice);

TypeId
PointToPointNetDevice::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::PointToPointNetDevice")
            .SetParent<NetDevice>()
            .SetGroupName("PointToPoint")
            .AddConstructor<PointToPointNetDevice>()
            .AddAttribute("Mtu",
                          "The MAC-level Maximum Transmission Unit",
                          UintegerValue(DEFAULT_MTU),
                          MakeUintegerAccessor(&PointToPointNetDevice::SetMtu,
                                               &PointToPointNetDevice::GetMtu),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("Address",
                          "The MAC address of this device.",
                          Mac48AddressValue(Mac48Address("ff:ff:ff:ff:ff:ff")),
                          MakeMac48AddressAccessor(&PointToPointNetDevice::m_address),
                          MakeMac48AddressChecker())
            .AddAttribute("DataRate",
                          "The default data rate for point to point links",
                          DataRateValue(DataRate("32768b/s")),
                          MakeDataRateAccessor(&PointToPointNetDevice::m_bps),
                          MakeDataRateChecker())
            .AddAttribute("ReceiveErrorModel",
                          "The receiver error model used to simulate packet loss",
                          PointerValue(),
                          MakePointerAccessor(&PointToPointNetDevice::m_receiveErrorModel),
                          MakePointerChecker<ErrorModel>())
            .AddAttribute("InterframeGap",
                          "The time to wait between packet (frame) transmissions",
                          TimeValue(Seconds(0)),
                          MakeTimeAccessor(&PointToPointNetDevice::m_tInterframeGap),
                          MakeTimeChecker())

            //
            // Transmit queueing discipline for the device which includes its own set
            // of trace hooks.
            //
            .AddAttribute("TxQueue",
                          "A queue to use as the transmit queue in the device.",
                          PointerValue(),
                          MakePointerAccessor(&PointToPointNetDevice::m_queue),
                          MakePointerChecker<PointToPointQueue>())

            //
            // Trace sources at the "top" of the net device, where packets transition
            // to/from higher layers.
            //
            .AddTraceSource("MacTx",
                            "Trace source indicating a packet has arrived "
                            "for transmission by this device",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_macTxTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("MacTxDrop",
                            "Trace source indicating a packet has been dropped "
                            "by the device before transmission",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_macTxDropTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("MacPromiscRx",
                            "A packet has been received by this device, "
                            "has been passed up from the physical layer "
                            "and is being forwarded up the local protocol stack.  "
                            "This is a promiscuous trace,",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_macPromiscRxTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("MacRx",
                            "A packet has been received by this device, "
                            "has been passed up from the physical layer "
                            "and is being forwarded up the local protocol stack.  "
                            "This is a non-promiscuous trace,",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_macRxTrace),
                            "ns3::Packet::TracedCallback")
#if 0
    // Not currently implemented for this device
    .AddTraceSource ("MacRxDrop",
                     "Trace source indicating a packet was dropped "
                     "before being forwarded up the stack",
                     MakeTraceSourceAccessor (&PointToPointNetDevice::m_macRxDropTrace),
                     "ns3::Packet::TracedCallback")
#endif
            //
            // Trace sources at the "bottom" of the net device, where packets transition
            // to/from the channel.
            //
            .AddTraceSource("PhyTxBegin",
                            "Trace source indicating a packet has begun "
                            "transmitting over the channel",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_phyTxBeginTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("PhyTxEnd",
                            "Trace source indicating a packet has been "
                            "completely transmitted over the channel",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_phyTxEndTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("PhyTxDrop",
                            "Trace source indicating a packet has been "
                            "dropped by the device during transmission",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_phyTxDropTrace),
                            "ns3::Packet::TracedCallback")
#if 0
    // Not currently implemented for this device
    .AddTraceSource ("PhyRxBegin",
                     "Trace source indicating a packet has begun "
                     "being received by the device",
                     MakeTraceSourceAccessor (&PointToPointNetDevice::m_phyRxBeginTrace),
                     "ns3::Packet::TracedCallback")
#endif
            .AddTraceSource("PhyRxEnd",
                            "Trace source indicating a packet has been "
                            "completely received by the device",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_phyRxEndTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("PhyRxDrop",
                            "Trace source indicating a packet has been "
                            "dropped by the device during reception",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_phyRxDropTrace),
                            "ns3::Packet::TracedCallback")

            //
            // Trace sources designed to simulate a packet sniffer facility (tcpdump).
            // Note that there is really no difference between promiscuous and
            // non-promiscuous traces in a point-to-point link.
            //
            .AddTraceSource("Sniffer",
                            "Trace source simulating a non-promiscuous packet sniffer "
                            "attached to the device",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_snifferTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("PromiscSniffer",
                            "Trace source simulating a promiscuous packet sniffer "
                            "attached to the device",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_promiscSnifferTrace),
                            "ns3::Packet::TracedCallback");
    return tid;
}

PointToPointNetDevice::PointToPointNetDevice()
    : m_txMachineState(READY),
      m_channel(nullptr),
      m_linkUp(false),
      m_currentPkt(nullptr)
{
    NS_LOG_FUNCTION(this);
}

PointToPointNetDevice::~PointToPointNetDevice()
{
    NS_LOG_FUNCTION(this);
}

void
PointToPointNetDevice::AddHeader(Ptr<Packet> p, uint16_t protocolNumber)
{
    NS_LOG_FUNCTION(this << p << protocolNumber);
    PppHeader ppp;
    ppp.SetProtocol(EtherToPpp(protocolNumber));
    p->AddHeader(ppp);
}

bool
PointToPointNetDevice::ProcessHeader(Ptr<Packet> p, uint16_t& param)
{
    NS_LOG_FUNCTION(this << p << param);
    PppHeader ppp;
    p->RemoveHeader(ppp);
    param = PppToEther(ppp.GetProtocol());
    return true;
}

void
PointToPointNetDevice::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_node = nullptr;
    m_channel = nullptr;
    m_receiveErrorModel = nullptr;
    m_currentPkt = nullptr;
    m_queue = nullptr;
    NetDevice::DoDispose();
}

void
PointToPointNetDevice::SetDataRate(DataRate bps)
{
    NS_LOG_FUNCTION(this);
    m_bps = bps;
}

DataRate
PointToPointNetDevice::GetDataRate() const
{
    return m_bps;
}

void
PointToPointNetDevice::SetInterframeGap(Time t)
{
    NS_LOG_FUNCTION(this << t.As(Time::S));
    m_tInterframeGap = t;
}

Time
PointToPointNetDevice::GetInterframeGap() const
{
    return m_tInterframeGap;
}

bool
PointToPointNetDevice::TransmitStart(Ptr<Packet> p)
{
    NS_LOG_FUNCTION(this << p);
    NS_LOG_LOGIC("UID is " << p->GetUid() << ")");

    if(m_type == NetDeviceType::SWITCH){
        PppHeader ppp;
        p->PeekHeader(ppp);
        uint16_t protocol = PppToEther(ppp.GetProtocol());
        p = DynamicCast<SwitchNode>(GetNode())->EgressPipeline(p, protocol, this);
    }

    //
    // This function is called to start the process of transmitting a packet.
    // We need to tell the channel that we've started wiggling the wire and
    // schedule an event that will be executed when the transmission is complete.
    //
    NS_ASSERT_MSG(m_txMachineState == READY, "Must be READY to transmit");
    m_txMachineState = BUSY;
    m_currentPkt = p;
    m_phyTxBeginTrace(m_currentPkt);

    if(p == nullptr){
        TransmitComplete();
        return true;
    }

    Time txTime = m_bps.CalculateBytesTxTime(p->GetSize());
    Time txCompleteTime = txTime + m_tInterframeGap;

    NS_LOG_LOGIC("Schedule TransmitCompleteEvent in " << txCompleteTime.As(Time::S));
    Simulator::Schedule(txCompleteTime, &PointToPointNetDevice::TransmitComplete, this);

    bool result = m_channel->TransmitStart(p, this, txTime);
    if (!result)
    {
        m_phyTxDropTrace(p);
    }
    return result;
}

void
PointToPointNetDevice::TransmitComplete()
{
    NS_LOG_FUNCTION(this);

    //
    // This function is called to when we're all done transmitting a packet.
    // We try and pull another packet off of the transmit queue.  If the queue
    // is empty, we are done, otherwise we need to start transmitting the
    // next packet.
    //
    NS_ASSERT_MSG(m_txMachineState == BUSY, "Must be BUSY if transmitting");
    m_txMachineState = READY;

    NS_ASSERT_MSG(m_currentPkt, "PointToPointNetDevice::TransmitComplete(): m_currentPkt zero");

    m_phyTxEndTrace(m_currentPkt);
    m_currentPkt = nullptr;

    Ptr<Packet> p = m_queue->Dequeue();
    if (!p)
    {
        CheckSendQueue();
        NS_LOG_LOGIC("No pending packets in device queue after tx complete");
        return;
    }

    //
    // Got another packet off of the queue, so start the transmit process again.
    //
    m_snifferTrace(p);
    m_promiscSnifferTrace(p);
    TransmitStart(p);
}

bool
PointToPointNetDevice::Attach(Ptr<PointToPointChannel> ch)
{
    NS_LOG_FUNCTION(this << &ch);

    m_channel = ch;

    m_channel->Attach(this);

    //
    // This device is up whenever it is attached to a channel.  A better plan
    // would be to have the link come up when both devices are attached, but this
    // is not done for now.
    //
    NotifyLinkUp();
    return true;
}

void
PointToPointNetDevice::SetQueue(Ptr<PointToPointQueue> q)
{
    NS_LOG_FUNCTION(this << q);
    m_queue = q;
}

void
PointToPointNetDevice::SetReceiveErrorModel(Ptr<ErrorModel> em)
{
    NS_LOG_FUNCTION(this << em);
    m_receiveErrorModel = em;
}

void
PointToPointNetDevice::Receive(Ptr<Packet> packet)
{
    NS_LOG_FUNCTION(this << packet);
    uint16_t protocol = 0;

    if (m_receiveErrorModel && m_receiveErrorModel->IsCorrupt(packet))
    {
        //
        // If we have an error model and it indicates that it is time to lose a
        // corrupted packet, don't forward this packet up, let it go.
        //
        m_phyRxDropTrace(packet);
    }
    else
    {
        //
        // Hit the trace hooks.  All of these hooks are in the same place in this
        // device because it is so simple, but this is not usually the case in
        // more complicated devices.
        //
        m_snifferTrace(packet);
        m_promiscSnifferTrace(packet);
        m_phyRxEndTrace(packet);

        //
        // Trace sinks will expect complete packets, not packets without some of the
        // headers.
        //
        Ptr<Packet> originalPacket = packet->Copy();

        //
        // Strip off the point-to-point protocol header and forward this packet
        // up the protocol stack.  Since this is a simple point-to-point link,
        // there is no difference in what the promisc callback sees and what the
        // normal receive callback sees.
        //
        ProcessHeader(packet, protocol);

        if(protocol == 0x8808){
            PfcHeader pfc;
            packet->RemoveHeader(pfc);
            m_queue->SetPauseFlag(pfc.GetQueueIndex(), pfc.GetTime() > 0);
            
            if(pfc.GetTime() == 0 && m_txMachineState == READY){
                Ptr<Packet> p = m_queue->Dequeue();
                if (p) TransmitStart(p);
                else CheckSendQueue();
            }
            return;
        }

        if(m_type == NetDeviceType::SERVER){
            Ipv4Header ipv4_header;
            UdpHeader udp_header;
            HpccHeader hpcc_header;
            BthHeader bth_header;

            packet->RemoveHeader(ipv4_header);
            packet->RemoveHeader(udp_header);
            if(m_ccVersion == 2){
                packet->RemoveHeader(hpcc_header);
            }
            packet->RemoveHeader(bth_header);

            uint32_t id = bth_header.GetId();
            if(bth_header.GetACK() || bth_header.GetNACK()){
                if(m_flows.find(id) != m_flows.end()){
                    if(m_flows[id]->ProcessACK(bth_header, hpcc_header)){
                        // Flow completed
                        m_flows.erase(id);
                        m_sendCompleted.erase(id);
                    }
                    else if(m_sendCompleted.find(id) != m_sendCompleted.end()){
                        auto qp = m_sendCompleted[id];
                        if(!qp->IsSendCompleted()){
                            m_sendQueue.emplace(qp->GetNextSendTime(), qp->GetId());
                            m_sendCompleted.erase(id);
                        }
                    }
                }
            }
            else{
                if(bth_header.GetSequence() <= m_receivers[id] + bth_header.GetSize()){
                    m_receivers[id] = bth_header.GetSequence();
                    Ptr<Packet> ackPacket = GenerateACK(ipv4_header, hpcc_header, bth_header, true);
                    Send(ackPacket, GetBroadcast(), 0x0800);
                }
                else{
                    bth_header.SetSequence(m_receivers[id]);
                    Ptr<Packet> nackPacket = GenerateACK(ipv4_header, hpcc_header, bth_header, false);
                    Send(nackPacket, GetBroadcast(), 0x0800);
                }
            }
            return;
        }

        if (!m_promiscCallback.IsNull())
        {
            m_macPromiscRxTrace(originalPacket);
            m_promiscCallback(this,
                              packet,
                              protocol,
                              GetRemote(),
                              GetAddress(),
                              NetDevice::PACKET_HOST);
        }

        m_macRxTrace(originalPacket);
        m_rxCallback(this, packet, protocol, GetRemote());
    }
}

Ptr<Packet> 
PointToPointNetDevice::GenerateACK(Ipv4Header ipv4_header, HpccHeader hpcc_header, BthHeader bth_header, bool isAck)
{
	Ptr<Packet> ret = Create<Packet>(0);

	if(isAck)
		bth_header.SetACK();
	else
		bth_header.SetNACK();
	if(ipv4_header.GetEcn() == Ipv4Header::EcnType::ECN_CE || !isAck)
		bth_header.SetCNP();
	bth_header.SetSize(0);
	ret->AddHeader(bth_header);

    if(m_ccVersion == 2){
        hpcc_header.StopAddIntHeader();
        ret->AddHeader(hpcc_header);
    }

	UdpHeader udp_header;
	udp_header.SetSourcePort(std::rand() % 65535);
	udp_header.SetDestinationPort(BthHeader::ROCE_UDP_PORT);
	ret->AddHeader(udp_header);

	Ipv4Header ipv4_hdr;
	ipv4_hdr.SetEcn(Ipv4Header::EcnType::ECN_ECT0);
	ipv4_hdr.SetPayloadSize(20);
	ipv4_hdr.SetProtocol(17);
	ipv4_hdr.SetTtl(64);
	ipv4_hdr.SetSource(ipv4_header.GetDestination());
	ipv4_hdr.SetDestination(ipv4_header.GetSource());
	ret->AddHeader(ipv4_hdr);

	SocketPriorityTag tag;
	tag.SetPriority(RdmaQueuePair::m_ackPriority);
	ret->ReplacePacketTag(tag);

	return ret;
}


Ptr<PointToPointQueue>
PointToPointNetDevice::GetQueue() const
{
    NS_LOG_FUNCTION(this);
    return m_queue;
}

void
PointToPointNetDevice::NotifyLinkUp()
{
    NS_LOG_FUNCTION(this);
    m_linkUp = true;
    m_linkChangeCallbacks();
}

void
PointToPointNetDevice::SetIfIndex(const uint32_t index)
{
    NS_LOG_FUNCTION(this);
    m_ifIndex = index;
}

uint32_t
PointToPointNetDevice::GetIfIndex() const
{
    return m_ifIndex;
}

Ptr<Channel>
PointToPointNetDevice::GetChannel() const
{
    return m_channel;
}

//
// This is a point-to-point device, so we really don't need any kind of address
// information.  However, the base class NetDevice wants us to define the
// methods to get and set the address.  Rather than be rude and assert, we let
// clients get and set the address, but simply ignore them.

void
PointToPointNetDevice::SetAddress(Address address)
{
    NS_LOG_FUNCTION(this << address);
    m_address = Mac48Address::ConvertFrom(address);
}

Address
PointToPointNetDevice::GetAddress() const
{
    return m_address;
}

bool
PointToPointNetDevice::IsLinkUp() const
{
    NS_LOG_FUNCTION(this);
    return m_linkUp;
}

void
PointToPointNetDevice::AddLinkChangeCallback(Callback<void> callback)
{
    NS_LOG_FUNCTION(this);
    m_linkChangeCallbacks.ConnectWithoutContext(callback);
}

//
// This is a point-to-point device, so every transmission is a broadcast to
// all of the devices on the network.
//
bool
PointToPointNetDevice::IsBroadcast() const
{
    NS_LOG_FUNCTION(this);
    return true;
}

//
// We don't really need any addressing information since this is a
// point-to-point device.  The base class NetDevice wants us to return a
// broadcast address, so we make up something reasonable.
//
Address
PointToPointNetDevice::GetBroadcast() const
{
    NS_LOG_FUNCTION(this);
    return Mac48Address::GetBroadcast();
}

bool
PointToPointNetDevice::IsMulticast() const
{
    NS_LOG_FUNCTION(this);
    return true;
}

Address
PointToPointNetDevice::GetMulticast(Ipv4Address multicastGroup) const
{
    NS_LOG_FUNCTION(this);
    return Mac48Address("01:00:5e:00:00:00");
}

Address
PointToPointNetDevice::GetMulticast(Ipv6Address addr) const
{
    NS_LOG_FUNCTION(this << addr);
    return Mac48Address("33:33:00:00:00:00");
}

bool
PointToPointNetDevice::IsPointToPoint() const
{
    NS_LOG_FUNCTION(this);
    return true;
}

bool
PointToPointNetDevice::IsBridge() const
{
    NS_LOG_FUNCTION(this);
    return false;
}

void
PointToPointNetDevice::CheckRetransmitQueue()
{
    while(!m_retransmitQueue.empty()){
        auto p = m_retransmitQueue.top();
        
        if(Simulator::Now().GetNanoSeconds() < p.first)
            break;

        m_retransmitQueue.pop();
        if(m_sendCompleted.find(p.second) == m_sendCompleted.end()){
            continue;
        }

        auto qp = m_sendCompleted[p.second];
        if(qp->GetTimeOut() != p.first)
            continue; // Already retransmitted, to avoid multiple retransmissions for the same timeout

        // std::cerr << "Retransmitting flow " << p.second << " at time " 
        //    << Simulator::Now().GetNanoSeconds() << " ns" << std::endl;
        qp->TimeOutReset();
        m_sendQueue.emplace(qp->GetNextSendTime(), p.second);
        m_sendCompleted.erase(p.second);
    }

    Simulator::Cancel(m_retransmitEvent);
    if(!m_retransmitQueue.empty()){
        auto p = m_retransmitQueue.top();
        m_retransmitEvent = Simulator::Schedule(NanoSeconds(p.first - Simulator::Now().GetNanoSeconds()),
                &PointToPointNetDevice::CheckRetransmitQueue, this);
    }
}

bool
PointToPointNetDevice::Send(Ptr<Packet> packet, const Address& dest, uint16_t protocolNumber)
{
    NS_LOG_FUNCTION(this << packet << dest << protocolNumber);
    NS_LOG_LOGIC("p=" << packet << ", dest=" << &dest);
    NS_LOG_LOGIC("UID is " << packet->GetUid());

    //
    // If IsLinkUp() is false it means there is no channel to send any packet
    // over so we just hit the drop trace on the packet and return an error.
    //
    if (!IsLinkUp())
    {
        m_macTxDropTrace(packet);
        return false;
    }

    //
    // Stick a point to point protocol header on the packet in preparation for
    // shoving it out the door.
    //
    AddHeader(packet, protocolNumber);

    m_macTxTrace(packet);

    //
    // We should enqueue and dequeue the packet to hit the tracing hooks.
    //
    if (m_queue->Enqueue(packet))
    {
        //
        // If the channel is ready for transition we send the packet right now
        //
        if (m_txMachineState == READY)
        {
            packet = m_queue->Dequeue();
            if(packet != nullptr){
                m_snifferTrace(packet);
                m_promiscSnifferTrace(packet);
                return TransmitStart(packet);
            }
            else{
                CheckSendQueue();
            }
        }
        return true;
    }

    // Enqueue may fail (overflow)

    m_macTxDropTrace(packet);
    return false;
}

bool
PointToPointNetDevice::SendFrom(Ptr<Packet> packet,
                                const Address& source,
                                const Address& dest,
                                uint16_t protocolNumber)
{
    NS_LOG_FUNCTION(this << packet << source << dest << protocolNumber);
    return false;
}

Ptr<Node>
PointToPointNetDevice::GetNode() const
{
    return m_node;
}

void
PointToPointNetDevice::SetNode(Ptr<Node> node)
{
    NS_LOG_FUNCTION(this);
    m_node = node;
}

bool
PointToPointNetDevice::NeedsArp() const
{
    NS_LOG_FUNCTION(this);
    return false;
}

void
PointToPointNetDevice::SetReceiveCallback(NetDevice::ReceiveCallback cb)
{
    m_rxCallback = cb;
}

void
PointToPointNetDevice::SetPromiscReceiveCallback(NetDevice::PromiscReceiveCallback cb)
{
    m_promiscCallback = cb;
}

void 
PointToPointNetDevice::CheckSendQueue(){
    if(m_sendQueue.empty() || m_type != NetDeviceType::SERVER ||
            m_txMachineState != READY || m_queue->GetPauseFlag(2))
        return;
    
    if(m_queue->Dequeue() != nullptr){
        std::cerr << "Queue should be empty when checking send queue!" << std::endl;
        return;
    }

    while(!m_sendQueue.empty()){
        auto p = m_sendQueue.top();
        if(Simulator::Now().GetNanoSeconds() < p.first)
            break;

        m_sendQueue.pop();
        if(m_flows.find(p.second) == m_flows.end())
            continue;
        auto qp = m_flows[p.second];

        Ptr<Packet> pkt = qp->GenerateNextPacket();

        if(qp->IsSendCompleted()){
            m_sendCompleted[p.second] = qp;
            m_retransmitQueue.emplace(qp->GetTimeOut(), p.second);
            Simulator::Cancel(m_retransmitEvent);
            m_retransmitEvent = Simulator::Schedule(NanoSeconds(std::min((int64_t)1, m_retransmitQueue.top().first - Simulator::Now().GetNanoSeconds())), 
                &PointToPointNetDevice::CheckRetransmitQueue, this);
        }
        else{
            m_sendQueue.emplace(qp->GetNextSendTime(), p.second);
        }

        if(pkt != nullptr){
            Send(pkt, GetBroadcast(), 0x0800);
            return;
        }
    }

    Simulator::Cancel(m_sendEvent);
    if(!m_sendQueue.empty()){
        auto p = m_sendQueue.top();
        m_sendEvent = Simulator::Schedule(NanoSeconds(p.first - Simulator::Now().GetNanoSeconds()),
                &PointToPointNetDevice::CheckSendQueue, this);
    }
}

void
PointToPointNetDevice::SetFlow(FlowInfo flow, FILE* logFilePtr, uint32_t ccVersion)
{
    if(m_flows.find(flow.id) != m_flows.end()){
        std::cerr << "Flow " << flow.id << " already exists!" << std::endl;
        return;
    }
    Ptr<RdmaQueuePair> qp = CreateObject<RdmaQueuePair>(flow, this, logFilePtr, ccVersion, m_pfcVersion);
    m_flows[flow.id] = qp;
    m_sendQueue.emplace(qp->GetNextSendTime(), qp->GetId());
    CheckSendQueue();
}

void
PointToPointNetDevice::SetCC(uint32_t ccVersion)
{
    m_ccVersion = ccVersion;
}

void
PointToPointNetDevice::SetPFC(uint32_t pfcVersion)
{
    m_pfcVersion = pfcVersion;
}

bool
PointToPointNetDevice::SupportsSendFrom() const
{
    NS_LOG_FUNCTION(this);
    return false;
}

void
PointToPointNetDevice::DoMpiReceive(Ptr<Packet> p)
{
    NS_LOG_FUNCTION(this << p);
    Receive(p);
}

Address
PointToPointNetDevice::GetRemote() const
{
    NS_LOG_FUNCTION(this);
    NS_ASSERT(m_channel->GetNDevices() == 2);
    for (std::size_t i = 0; i < m_channel->GetNDevices(); ++i)
    {
        Ptr<NetDevice> tmp = m_channel->GetDevice(i);
        if (tmp != this)
        {
            return tmp->GetAddress();
        }
    }
    NS_ASSERT(false);
    // quiet compiler.
    return Address();
}

bool
PointToPointNetDevice::SetMtu(uint16_t mtu)
{
    NS_LOG_FUNCTION(this << mtu);
    m_mtu = mtu;
    return true;
}

uint16_t
PointToPointNetDevice::GetMtu() const
{
    NS_LOG_FUNCTION(this);
    return m_mtu;
}

void
PointToPointNetDevice::SetId(uint32_t id)
{
    m_id = id;
}

void
PointToPointNetDevice::SetDeviceType(NetDeviceType type)
{
    m_type = type;
}

uint16_t
PointToPointNetDevice::PppToEther(uint16_t proto)
{
    NS_LOG_FUNCTION_NOARGS();
    switch (proto)
    {
    case 0x0021:
        return 0x0800; // IPv4
    case 0x0057:
        return 0x86DD; // IPv6
    case 0x8808:
        return 0x8808; // Ethernet flow control
    default:
        NS_ASSERT_MSG(false, "PPP Protocol number not defined!");
    }
    return 0;
}

uint16_t
PointToPointNetDevice::EtherToPpp(uint16_t proto)
{
    NS_LOG_FUNCTION_NOARGS();
    switch (proto)
    {
    case 0x0800:
        return 0x0021; // IPv4
    case 0x86DD:
        return 0x0057; // IPv6
    case 0x8808:
        return 0x8808; // Ethernet flow control
    default:
        NS_ASSERT_MSG(false, "PPP Protocol number not defined!");
    }
    return 0;
}

} // namespace ns3
