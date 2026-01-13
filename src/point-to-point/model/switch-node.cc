#include "switch-node.h"

#include "ns3/application.h"
#include "ns3/net-device.h"
#include "ns3/node-list.h"

#include "ns3/assert.h"
#include "ns3/boolean.h"
#include "ns3/global-value.h"
#include "ns3/log.h"
#include "ns3/object-vector.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/random-variable-stream.h"

#include "ns3/udp-header.h"
#include "ns3/udp-l4-protocol.h"
#include "ns3/socket.h"

#include "point-to-point-net-device.h"
#include "point-to-point-queue.h"
#include "point-to-point-channel.h"
#include "pfc-header.h"
#include "ppp-header.h"
#include "bubble-header.h"
#include "packet-tag.h"

#include <unordered_map>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("SwitchNode");

NS_OBJECT_ENSURE_REGISTERED(SwitchNode);

TypeId
SwitchNode::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::SwitchNode")
            .SetParent<Node>()
            .SetGroupName("PointToPoint")
            .AddConstructor<SwitchNode>();
    return tid;
}

SwitchNode::SwitchNode() : Node()
{
}

SwitchNode::~SwitchNode()
{
}

uint32_t
SwitchNode::AddDevice(Ptr<NetDevice> device)
{
    NS_LOG_FUNCTION(this << device);

    uint32_t index = GetNDevices();
    m_devices.push_back(device);
    device->SetNode(this);
    device->SetIfIndex(index);
    device->SetReceiveCallback(MakeCallback(&SwitchNode::ReceiveFromDevice, this));
    Simulator::ScheduleWithContext(GetId(), Seconds(0.0), &NetDevice::Initialize, device);
    NotifyDeviceAdded(device);
    Ptr<PointToPointNetDevice> ptpDev = DynamicCast<PointToPointNetDevice>(device);
    if(ptpDev){
        Ptr<PointToPointChannel> channel = DynamicCast<PointToPointChannel>(ptpDev->GetChannel());
        m_hdrmBuffer[ptpDev] = ptpDev->GetDataRate().GetBitRate() * channel->GetDelay().GetSeconds() / 8.0 * 3.0; // 3 RTT
        m_usedHdrm[ptpDev] = 0;
        m_usedIngress[ptpDev] = 0;
        m_usedEgress[ptpDev] = 0;
        m_bubbleRate[ptpDev] = 0;
        
        double shared = ptpDev->GetDataRate().GetBitRate() / 1e9 * 5000.0; // 5KB per Gbps
        m_bufferTotal += shared;
        m_hdrmTotal += m_hdrmBuffer[ptpDev];
        m_reservedTotal += RESERVED_SIZE;
        m_sharedTotal += shared - RESERVED_SIZE - m_hdrmBuffer[ptpDev];
        if(shared - RESERVED_SIZE - m_hdrmBuffer[ptpDev] < 0){
            std::cout << "Warning: Negative shared buffer in Switch " << m_nid << std::endl;
        }

        m_kmin[ptpDev] = 0.1 * shared;
        m_kmax[ptpDev] = 0.4 * shared;
    }
    return index;
}

bool
SwitchNode::ReceiveFromDevice(Ptr<NetDevice> device,
                                  Ptr<const Packet> p,
                                  uint16_t protocol,
                                  const Address& from)
{
    Ptr<Packet> packet = p->Copy();
    return IngressPipeline(packet, protocol, DynamicCast<PointToPointNetDevice>(device));
}

void
SwitchNode::SetECMPHash(uint32_t hashSeed)
{
    m_hashSeed = hashSeed;
}

void
SwitchNode::SetPFC(uint32_t pfc)
{
    m_pfc = pfc;
}

void
SwitchNode::SetCC(uint32_t cc)
{
    m_cc = cc;
}

void
SwitchNode::SetId(uint32_t id)
{
    m_nid = id;
    m_uniformVar.SetStream(id);
}

uint32_t
SwitchNode::GetId()
{
    return m_nid;
}

void
SwitchNode::SetOutput(std::string output)
{
    m_output = output;
    std::string out_file = m_output + ".node";
    FILE* fout = fopen(out_file.c_str(), "w");
    fclose(fout);
}

void
SwitchNode::AddHostRouteTo(uint32_t dst, uint32_t devId)
{
    m_route[dst].push_back(devId);
}

Ptr<Packet>
SwitchNode::EgressPipeline(Ptr<Packet> packet, uint16_t protocol, Ptr<PointToPointNetDevice> dev){
    if(protocol != 0x0800)
        return packet;

    PppHeader ppp;
    packet->RemoveHeader(ppp);

    PacketTag packetTag;
    if(!packet->PeekPacketTag(packetTag))
        std::cerr << "Fail to find packetTag" << std::endl;

    Ptr<PointToPointNetDevice> ingressDev = packetTag.GetNetDevice();

    m_usedEgress[dev] -= packetTag.GetSize();
    if(m_usedEgress[dev] < 0){
        std::cout << "Error for usedEgress in Switch " << m_nid << std::endl;
        std::cout << "Egress size : " << m_usedEgress[dev] << std::endl;
    }

    int32_t fromHdrm = std::min((int32_t)packetTag.GetSize(), m_usedHdrm[ingressDev]);
    m_usedHdrm[ingressDev] -= fromHdrm;
    if(m_usedHdrm[ingressDev] < 0){
        std::cout << "Error for usedHdrm in Switch " << m_nid << std::endl;
        std::cout << "Egress size : " << m_usedHdrm[ingressDev] << std::endl;
    }

    int remain = packetTag.GetSize() - fromHdrm;

    m_usedShared -= std::min(remain, std::max(0, m_usedIngress[ingressDev] - RESERVED_SIZE));
    if(m_usedShared < 0){
        std::cout << "Error for usedShared in Switch " << m_nid << std::endl;
        std::cout << "Egress size : " << m_usedShared << std::endl;
    }

    m_usedIngress[ingressDev] -= remain;
    if(m_usedIngress[ingressDev] < 0){
        std::cout << "Error for usedIngress in Switch " << m_nid << std::endl;
        std::cout << "Egress size : " << m_usedIngress[ingressDev] << std::endl;
    }

    packet->AddHeader(ppp);

    if(ShouldResume(ingressDev)){
        SendPFC(ingressDev, false);
    }
    if(m_pfc == 2){
        CheckBubble(ingressDev);
    }
    return packet;
}

bool
SwitchNode::IngressPipeline(Ptr<Packet> packet, uint16_t protocol, Ptr<PointToPointNetDevice> dev){
    if(protocol != 0x0800){ // IPv4
        std::cout << "Drop non-IPv4 packet in Switch " << m_nid << std::endl;
        return false;
    }

    // if(m_uniformVar.GetValue(0.0, 1.0) < 0.01)
    //    return false;

    // Drop check
    if((int32_t)packet->GetSize() + m_usedHdrm[dev] > m_hdrmBuffer[dev] && 
       (int32_t)packet->GetSize() + GetUsedShared(dev) > GetSharedThreshold(dev)){
        m_drops += 1;
        if(m_pfc != 0){
            std::cerr << "Drop packet in Switch " << m_nid << " under PFC mode" << std::endl;
        }
        if(m_drops % 10000 == 0){
            std::cerr << "Switch " << m_nid << " drop count: " << m_drops << std::endl;
        }
        return false;
    }

    // Routing
    Ipv4Header ipv4_header;
    UdpHeader udp_header;

    packet->RemoveHeader(ipv4_header);
    packet->RemoveHeader(udp_header);

    uint8_t ttl = ipv4_header.GetTtl();
    if(ttl == 0){
        std::cout << "TTL = 0 for IP in Switch" << std::endl;
        return false;
    }
    ipv4_header.SetTtl(ttl - 1);

    const std::vector<uint32_t>& route_vec = m_route[ipv4_header.GetDestination().Get()];
    if(route_vec.size() == 0){
        std::cout << "Fail to get next dev" << std::endl;
        return false;
    }

    FlowV4Id id = FlowV4Id(ipv4_header.GetSource().Get(),
                           ipv4_header.GetDestination().Get(),
                           udp_header.GetSourcePort(),
                           udp_header.GetDestinationPort());

    uint32_t hashValue = 0;
    if(route_vec.size() > 1)
        hashValue = id.hash(m_hashSeed);
    uint32_t devId = route_vec[hashValue % route_vec.size()];
    if(devId >= GetNDevices()){
        std::cout << "Error devId in SwitchNode" << std::endl;
        return false;
    }

    packet->AddHeader(udp_header);
    packet->AddHeader(ipv4_header);


    // Buffer update    
    Ptr<PointToPointNetDevice> egressDev = DynamicCast<PointToPointNetDevice>(GetDevice(devId));
    if(egressDev == nullptr){
        std::cout << "Fail to get PointToPointNetDevice in SwitchNode" << std::endl;
        return false;
    }

    PacketTag packetTag;
    packetTag.SetSize(packet->GetSize());
    packetTag.SetNetDevice(dev);

    m_usedEgress[egressDev] += packet->GetSize();

    int32_t newBytes = packet->GetSize() + m_usedIngress[dev];
    if(newBytes <= RESERVED_SIZE){
        m_usedIngress[dev] = newBytes;
    }
    else {
        int32_t thresh = GetSharedThreshold(dev);
		if(newBytes - RESERVED_SIZE > thresh){
			m_usedHdrm[dev] += packet->GetSize();
		}
        else{
            m_usedIngress[dev] = newBytes;
            int32_t toShared = std::min((int32_t)packet->GetSize(), newBytes - RESERVED_SIZE);
            m_usedShared += toShared;
		}
    }

    packet->ReplacePacketTag(packetTag);


    if(ShouldPause(dev)){
        SendPFC(dev, true);
    }

    if(m_pfc == 2){
        CheckBubble(dev);
    }

    if(ShouldECN(egressDev)){
        m_ecnCount += 1;
        packet->RemoveHeader(ipv4_header);
        ipv4_header.SetEcn(Ipv4Header::ECN_CE);
        packet->AddHeader(ipv4_header);
    }

    // Send packet
    if(!egressDev->Send(packet, egressDev->GetBroadcast(), protocol)){
        std::cout << "Fail to send packet in SwitchNode" << std::endl;
        return false;
    }
    return true;
}

int32_t 
SwitchNode::GetSharedThreshold(Ptr<PointToPointNetDevice> dev)
{
    return m_bufferTotal - m_reservedTotal - m_hdrmTotal - m_usedShared;
}

int32_t 
SwitchNode::GetUsedShared(Ptr<PointToPointNetDevice> dev)
{
    if(m_usedIngress[dev] > RESERVED_SIZE)
        return m_usedIngress[dev] - RESERVED_SIZE;
    return 0;
}

bool
SwitchNode::ShouldECN(Ptr<PointToPointNetDevice> dev)
{
    if(m_usedEgress[dev] < m_kmin[dev])
        return false;
    if(m_usedEgress[dev] > m_kmax[dev])
        return true;
    double prob = 0.2 * (m_usedEgress[dev] - m_kmin[dev]) / (m_kmax[dev] - m_kmin[dev]);
    double rand_val = m_uniformVar.GetValue(0.0, 1.0);
    return rand_val < prob;
}

bool
SwitchNode::ShouldPause(Ptr<PointToPointNetDevice> dev)
{
    if(m_pfc != 1 || m_pause[dev])
        return false;
    if(m_usedHdrm[dev] > 0 || GetUsedShared(dev) >= GetSharedThreshold(dev)){
        m_pause[dev] = true;
        return true;
    }
    return false;
}

bool 
SwitchNode::ShouldResume(Ptr<PointToPointNetDevice> dev)
{
    if(!m_pause[dev])
        return false;
    int32_t sharedUsed = GetUsedShared(dev);
    if(m_usedHdrm[dev] == 0 && (sharedUsed == 0 || sharedUsed + RESUME_OFFSET <= GetSharedThreshold(dev))){
        m_pause[dev] = false;
        return true;
    }
    return false;
}

void
SwitchNode::CheckBubble(Ptr<PointToPointNetDevice> dev)
{
    uint8_t newRate = 0;
    int32_t sharedUsed = GetUsedShared(dev);
    int32_t thresh = GetSharedThreshold(dev);
    if(m_usedHdrm[dev] > 0 || sharedUsed >= thresh){
        newRate = 8;
    }
    else if(sharedUsed == 0){
        newRate = 0;
    }
    else if(Simulator::Now().GetNanoSeconds() - m_bubbleTime[dev] < 10000){ // 10us
        return;
    }
    else{
        double total = dev->GetDataRate().GetBitRate() / 1e9 * 5000.0 - m_hdrmBuffer[dev];
        double target = total * 0.1; // target 10% usage

        double rate = (m_usedIngress[dev] - m_prevBuffer[dev]) * 8.0 / 1e-5 + (m_usedIngress[dev] - target) * 8.0/ 1e-4;
        double ratio = rate * 8 / dev->GetDataRate().GetBitRate();
        if(ratio > 7.0)
            newRate = 7;
        else if(ratio < 0.0)
            newRate = 0;
        else
            newRate = (uint8_t)ratio;
    }

    m_prevBuffer[dev] = m_usedIngress[dev];
    m_bubbleTime[dev] = Simulator::Now().GetNanoSeconds();

    if(newRate != m_bubbleRate[dev]){
        m_bubbleRate[dev] = newRate;
        // std::cout << "Switch " << m_nid << " set bubble rate " << (uint32_t)newRate << " for dev" << std::endl;
        Ptr<Packet> packet = Create<Packet>();
        BubbleHeader bubbleHeader;
        bubbleHeader.SetBubbleRate(newRate);
        packet->AddHeader(bubbleHeader);
        if(!dev->Send(packet, dev->GetBroadcast(), 0x4321))
            std::cout << "Drop of Bubble Rate Update" << std::endl;
    }
}

void 
SwitchNode::SendPFC(Ptr<NetDevice> dev, bool pause)
{
    // std::cout << "Send PFC from Switch " << m_nid << std::endl;
    Ptr<Packet> packet = Create<Packet>();
    PfcHeader pfc_header;
    pfc_header.SetTime(pause);
    pfc_header.SetQueueIndex(2);
    pfc_header.SetQueueSize(0);
    packet->AddHeader(pfc_header);

    if(!dev->Send(packet, dev->GetBroadcast(), 0x8808))
        std::cout << "Drop of PFC" << std::endl;
}

} // namespace ns3