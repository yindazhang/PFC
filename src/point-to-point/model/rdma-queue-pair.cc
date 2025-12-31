#include "ns3/node.h"
#include "ns3/socket.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/uinteger.h"
#include "ns3/udp-header.h"
#include "ns3/bth-header.h"
#include "ns3/ipv4-header.h"

#include "rdma-queue-pair.h"
#include "hpcc-header.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RdmaQueuePair");

NS_OBJECT_ENSURE_REGISTERED(RdmaQueuePair);

TypeId
RdmaQueuePair::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RdmaQueuePair")
                            .SetParent<Object>()
                            .SetGroupName("PointToPoint")
							.AddAttribute("SendSize",
								"The amount of data to send each time.",
								UintegerValue(4000),
								MakeUintegerAccessor(&RdmaQueuePair::m_sendSize),
								MakeUintegerChecker<uint32_t>());
    return tid;
}

RdmaQueuePair::RdmaQueuePair(FlowInfo flow, Ptr<PointToPointNetDevice> device, FILE* logFilePtr, uint32_t ccVersion, uint32_t pfcVersion)
        : m_flow(flow), m_device(device), m_logFile(logFilePtr), m_ccVersion(ccVersion), m_pfcVersion(pfcVersion){
	m_port = (flow.id & 0xFFFF);
	m_maxRate = device->GetDataRate();
	m_currentRate = m_maxRate;
	m_mlxTargetRate = m_currentRate;
	m_lastSendTime = 0;
	m_lastGenerateTime = 0;
};

int64_t 
RdmaQueuePair::GetNextSendTime()
{
	return m_lastGenerateTime + (m_sendSize * 8.0 * 1e9 / m_currentRate.GetBitRate());
}

bool RdmaQueuePair::IsSendCompleted() const 
{ 
	return m_bytesSent >= m_flow.size; 
}

int64_t 
RdmaQueuePair::GetTimeOut()
{
	if(!IsSendCompleted())
		std::cerr << "GetTimeOut called for non-completed flow!" << std::endl;
	return m_lastSendTime + 2000000; // 2 milliseconds
}

void 
RdmaQueuePair::TimeOutReset()
{
	m_port += 1; // change port for load balancing
	m_bytesSent = m_bytesAcked;
	m_lastSendTime = m_lastGenerateTime = Simulator::Now().GetNanoSeconds();
	if(m_ccVersion == 1){
		DecreaseMlxRate();
	}
	if(m_pfcVersion == 1){
		std::cerr << "Timeout reset for flow " << m_flow.id << ", retransmitting from byte " << m_bytesSent << std::endl;
	}
}

bool 
RdmaQueuePair::ProcessACK(BthHeader& bth_header, HpccHeader& hpcc_header)
{
	if(bth_header.GetId() != m_flow.id){
		std::cerr << "ACK for unknown flow id " << bth_header.GetId() << std::endl;
		return false;
	}

	if(m_ccVersion == 2){
		// HPCC RDMA congestion control
		std::vector<IntHeader> intHeaders = hpcc_header.GetIntHeaders();
		std::cout << "Flow " << m_flow.id << " received HPCC INT headers from " << (uint32_t)intHeaders.size() << " hops." << std::endl;
		for(auto& intHeader : intHeaders){
			std::cout << "  Rate: " << intHeader.GetRate().GetBitRate() / 1e6 << " Mbps, "
					  << "Bytes: " << intHeader.GetBytes() << ", "
					  << "QueueLen: " << intHeader.GetQueueLen() << std::endl;
		}
	}

	uint32_t seq = bth_header.GetSequence();
	m_bytesAcked = std::max(m_bytesAcked, seq);

	if(bth_header.GetACK()){
		if(m_bytesAcked > m_bytesSent){
			m_bytesSent = m_bytesAcked;
		}
		if(m_bytesAcked >= m_flow.size){
			WriteFCT();
			if(m_ccVersion == 1){
				Simulator::Cancel(m_mlxUpdateAlpha);
				Simulator::Cancel(m_mlxIncreaseRate);
			}
			return true;
		}
	}
	else if(bth_header.GetNACK()){
		m_bytesSent = m_bytesAcked;
	}
	else{
		std::cerr << "Unknown ACK type" << std::endl;
		return false;
	}

	if(bth_header.GetCNP()){
		if(m_ccVersion == 1){
			DecreaseMlxRate();
		}
	}

	return false;
}

Ptr<Packet>
RdmaQueuePair::GenerateNextPacket()
{
	if(IsSendCompleted()){
		std::cerr << "All data already sent for flow " << m_flow.id << std::endl;
		return nullptr;
	}

	/*
	if(m_flow.id == 99999999){
		std::cerr << "Generating packet for flow " << m_flow.id << " at time " 
			<< Simulator::Now().GetNanoSeconds() << " ns, bytes sent " << m_bytesSent << " , bytes acked " << m_bytesAcked << std::endl;
		std::cerr << "Current rate " << m_currentRate.GetBitRate() / 1e6 << " Mbps with flow size " << m_flow.size << " bytes." << std::endl;
		std::cerr << "Last send time " << m_lastSendTime << " ns, last generate time " << m_lastGenerateTime << " ns." << std::endl;
	}
	*/

	m_lastGenerateTime = Simulator::Now().GetNanoSeconds();

	if(m_lastSendTime != 0 && m_lastGenerateTime - m_lastSendTime > 2000000){ // 2 milliseconds
		m_port += 1; // change port for load balancing
		m_bytesSent = m_bytesAcked;
		if(m_ccVersion == 1){
			DecreaseMlxRate();
		}
		if(m_pfcVersion == 1){
			std::cerr << "Timeout detected for flow " << m_flow.id << ", retransmitting from byte " << m_bytesSent << std::endl;
		}
	}
	else{
		uint32_t inFlight = m_bytesSent - m_bytesAcked;
		if(inFlight * 8 >= std::max(800000.0, m_currentRate.GetBitRate() * 0.0002)){ // 200 microseconds bandwidth-delay product
			return nullptr;
		}
	}

	m_lastSendTime = Simulator::Now().GetNanoSeconds();

	uint32_t toSend = std::min(m_flow.size - m_bytesSent, m_sendSize);
	Ptr<Packet> ret = Create<Packet>(toSend);

	BthHeader bth_header;
	bth_header.SetSize(toSend);
	bth_header.SetId(m_flow.id);
	bth_header.SetSequence(m_bytesSent + toSend);
	ret->AddHeader(bth_header);

	// HPCC RDMA congestion control
	if(m_ccVersion == 2){
		HpccHeader hpcc_header;
		ret->AddHeader(hpcc_header);
	}

	UdpHeader udp_header;
	udp_header.SetSourcePort(m_port);
	udp_header.SetDestinationPort(BthHeader::ROCE_UDP_PORT);
	ret->AddHeader(udp_header);

	Ipv4Header ipv4_header;
	ipv4_header.SetEcn(Ipv4Header::EcnType::ECN_ECT0);
	ipv4_header.SetPayloadSize(toSend + 20);
	ipv4_header.SetProtocol(17);
	ipv4_header.SetTtl(64);
	ipv4_header.SetSource(Ipv4Address(m_flow.src));
	ipv4_header.SetDestination(Ipv4Address(m_flow.dst));
	ret->AddHeader(ipv4_header);

	SocketPriorityTag tag;
	tag.SetPriority(m_dataPriority);
	ret->ReplacePacketTag(tag);

	m_bytesSent += toSend;
	return ret;
}

void 
RdmaQueuePair::WriteFCT(){
	if(m_flow.endTime == 0){
		m_flow.endTime = Simulator::Now().GetNanoSeconds();
		fprintf(m_logFile, "%u,%u,%u,%u,%lu,%lu,%lu\n",
			m_flow.id, m_flow.src, m_flow.dst,
			m_flow.size, m_flow.startTime, m_flow.endTime,
			m_flow.endTime - m_flow.startTime
		);
		fflush(m_logFile);
	}
}

void
RdmaQueuePair::DecreaseMlxRate(){
	m_mlxCnpAlpha = true;
	// MLX congestion control
	if(Simulator::Now().GetNanoSeconds() - m_prevCnpTime > 40000){ // 40 microseconds
		m_prevCnpTime = Simulator::Now().GetNanoSeconds();
		m_mlxTargetRate = m_currentRate;
		m_currentRate = std::max(m_minRate, m_currentRate * (1.0 - m_mlxAlpha / 2.0));
	}
	UpdateMlxAlpha();
	m_mlxTimeStage = 0;
	Simulator::Cancel(m_mlxIncreaseRate);
	m_mlxIncreaseRate = Simulator::Schedule(MicroSeconds(50), &RdmaQueuePair::IncreaseMlxRate, this);
}

void
RdmaQueuePair::UpdateMlxAlpha(){
	Simulator::Cancel(m_mlxUpdateAlpha);
	if(m_mlxCnpAlpha){
		m_mlxAlpha = (1 - m_mlxG) * m_mlxAlpha + m_mlxG;
	}
	else{
		m_mlxAlpha = (1 - m_mlxG) * m_mlxAlpha;
	}
	m_mlxCnpAlpha = false;
	m_mlxUpdateAlpha = Simulator::Schedule(MicroSeconds(45), &RdmaQueuePair::UpdateMlxAlpha, this);
}

void
RdmaQueuePair::IncreaseMlxRate(){
	Simulator::Cancel(m_mlxIncreaseRate);
	if(m_mlxTimeStage > 0){
		m_mlxTargetRate = std::min(m_maxRate, m_mlxTargetRate + DataRate("0.1Gbps")); // increase by 0.1 Gbps
	}
	m_currentRate = (m_mlxTargetRate + m_currentRate) * 0.5;
	m_mlxTimeStage += 1;
	m_mlxIncreaseRate = Simulator::Schedule(MicroSeconds(50), &RdmaQueuePair::IncreaseMlxRate, this);
}

} // namespace ns3