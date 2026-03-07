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
	// Add propagation delay to RTT estimation
	m_flow.minRttNs += m_sendSize * 8 * 1e9 / device->GetDataRate().GetBitRate();

	m_port = (flow.id & 0xFFFF);

	m_maxRate = device->GetDataRate();
	m_minRate = DataRate(m_sendSize * 8 * 1e9 / m_flow.minRttNs / 2.0); // at least enough to keep one packet in flight
	m_increase = m_minRate;

	m_currentRate = m_maxRate;
	m_win = m_currentRate.GetBitRate() / 1e9 * m_flow.minRttNs;

	m_mlxTargetRate = m_currentRate;
	m_hpccPrevRate = m_currentRate;
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
	if(m_ccVersion == 1)
		DecreaseMlxRate();
	if(m_pfcVersion == 1)
		std::cerr << "Timeout reset for flow " << m_flow.id << ", retransmitting from byte " << m_bytesSent << std::endl;
}

bool 
RdmaQueuePair::ProcessACK(BthHeader& bth_header, HpccHeader& hpcc_header)
{
	if(bth_header.GetId() != m_flow.id){
		std::cerr << "ACK for unknown flow id " << bth_header.GetId() << std::endl;
		return false;
	}

	uint32_t seq = bth_header.GetSequence();
	bool newACK = seq > m_bytesAcked;
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

		if(newACK){
			if(m_ccVersion == 3){
				ProcessDctcpACK(m_bytesAcked, bth_header.GetCNP());
			}
			else if(m_ccVersion == 4){
				ProcessNewDctcpACK(m_bytesAcked, bth_header.GetCNP());
			}
		}
	}
	else if(bth_header.GetNACK()){
		m_bytesSent = m_bytesAcked;
		std::cerr << "NACK received for flow " << m_flow.id << ", retransmitting from byte " << m_bytesSent << std::endl;
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

	if(m_ccVersion == 2 && newACK){
		// HPCC congestion control
		if(m_hpccLastSeq == 0){
			m_hpccLastSeq = m_bytesSent + 1;
			m_hpccHeaders = hpcc_header.GetIntHeaders();
			return false;
		}

		auto newHeaders = hpcc_header.GetIntHeaders();
		if(newHeaders.size() != m_hpccHeaders.size()){
			std::cerr << "Inconsistent number of INT headers for flow " << m_flow.id
					  << ": previous " << m_hpccHeaders.size()
					  << ", current " << newHeaders.size() << std::endl;
			return false;
		}

		UpdateHpccRate(newHeaders, seq > m_hpccLastSeq);
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
	if(m_flow.id == 1){
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
		if(m_ccVersion == 1)
			DecreaseMlxRate();
		if(m_pfcVersion == 1)
			std::cerr << "Timeout detected for flow " << m_flow.id << ", retransmitting from byte " << m_bytesSent << std::endl;
	}
	else{
		uint32_t inFlight = m_bytesSent - m_bytesAcked;
		if(m_ccVersion == 4){
			// std::cout << "Flow " << m_flow.id << " in-flight " << inFlight << " bytes, window " << m_win / 8 << " bytes." << std::endl;
			if(inFlight * 8 >= std::max(m_sendSize * 8 * 1.5, (double)m_win)){
				// std::cout << "Flow " << m_flow.id << " in-flight " << inFlight << " bytes, window " << m_win / 8 << " bytes." << std::endl;
				// std::cout << "Flow " << m_flow.id << " is limited by window, not sending new packet." << std::endl;
				return nullptr;
			}
		}
		else{
			if(inFlight * 8 >= std::max(m_sendSize * 8 * 1.5, m_currentRate.GetBitRate() / 1e9 * m_flow.minRttNs)){ 
				return nullptr;
			}
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
RdmaQueuePair::ProcessDctcpACK(uint32_t ackedBytes, bool cnp){
	m_dctcpEcnCount += cnp;

	if(m_dctcpCongested && ackedBytes > m_dctcpLastEcn){
		m_dctcpCongested = false;
	}

	if(ackedBytes > m_dctcpLastSeq){
		if(m_dctcpLastSeq == 0){
			m_dctcpAlphaSize = std::max(1U, (m_bytesSent + m_sendSize - 1) / m_sendSize);
		}
		else{
			double frac = std::min(1.0, double(m_dctcpEcnCount) / double(m_dctcpAlphaSize));
			m_alpha = (1 - m_g) * m_alpha + m_g * frac;
			m_dctcpEcnCount = 0;
			m_dctcpAlphaSize = std::max(1U, (m_bytesSent - ackedBytes + m_sendSize - 1) / m_sendSize);
		}

		if(!m_dctcpCongested && !cnp){
			m_currentRate = std::min(m_maxRate, m_currentRate + m_increase);
		}
		m_dctcpLastSeq = m_bytesSent + 1;
	}

	if(cnp && !m_dctcpCongested){
		m_dctcpCongested = true;
		m_dctcpLastEcn = m_bytesSent + 1;
		m_currentRate = std::max(m_minRate, m_currentRate * (1.0 - m_alpha / 2.0));
	}
}

void
RdmaQueuePair::ProcessNewDctcpACK(uint32_t ackedBytes, bool cnp){
	m_dctcpEcnCount += cnp;

	if(m_dctcpCongested && ackedBytes > m_dctcpLastEcn){
		m_dctcpCongested = false;
	}

	// std::cout << "Flow " << m_flow.id << " ACKed " << ackedBytes << " bytes, last seq " << m_dctcpLastSeq << std::endl;
	if(ackedBytes > m_dctcpLastSeq){
		if(m_dctcpLastSeq == 0){
			m_dctcpAlphaSize = std::max(1U, (m_bytesSent + m_sendSize - 1) / m_sendSize);
		}
		else{
			double frac = std::min(1.0, double(m_dctcpEcnCount) / double(m_dctcpAlphaSize));
			m_alpha = (1 - m_g) * m_alpha + m_g * frac;
			m_dctcpEcnCount = 0;
			m_dctcpAlphaSize = std::max(1U, (m_bytesSent - ackedBytes + m_sendSize - 1) / m_sendSize);
		}

		if(!m_dctcpCongested && !cnp){
			// std::cout << "Flow " << m_flow.id << " ACKed " << ackedBytes << " bytes, increasing rate." << std::endl;
			m_currentRate = std::min(m_maxRate, m_currentRate + m_increase + m_increase);
		}
		m_win = m_win + m_increase.GetBitRate() / 1e9 * m_flow.minRttNs;
		m_win = std::min((double)m_win, m_maxRate.GetBitRate() / 1e9 * m_flow.minRttNs);
		m_dctcpLastSeq = m_bytesSent + 1;
	}

	if(cnp && !m_dctcpCongested){
		// std::cout << "Flow " << m_flow.id << " received CNP, decreasing rate." << std::endl;
		m_dctcpCongested = true;
		m_dctcpLastEcn = m_bytesSent + 1;
		m_currentRate = std::max(m_minRate, m_currentRate * (1.0 - m_alpha / 2.0));
		uint64_t window = m_currentRate.GetBitRate() / 1e9 * m_flow.minRttNs;
		m_win = std::min(m_win, window);
	}
}

void
RdmaQueuePair::DecreaseMlxRate(){
	m_mlxCnpAlpha = true;
	// MLX congestion control
	UpdateMlxAlpha();
	if(Simulator::Now().GetNanoSeconds() - m_prevCnpTime >= m_flow.minRttNs){
		m_prevCnpTime = Simulator::Now().GetNanoSeconds();
		m_mlxTargetRate = m_currentRate;
		m_currentRate = std::max(m_minRate, m_currentRate * (1.0 - m_alpha / 2.0));
	}
	m_mlxTimeStage = 0;
	Simulator::Cancel(m_mlxIncreaseRate);
	m_mlxIncreaseRate = Simulator::Schedule(NanoSeconds(m_flow.minRttNs * 2), &RdmaQueuePair::IncreaseMlxRate, this);
}

void
RdmaQueuePair::UpdateMlxAlpha(){
	Simulator::Cancel(m_mlxUpdateAlpha);
	if(m_mlxCnpAlpha){
		m_alpha = (1 - m_g) * m_alpha + m_g;
	}
	else{
		m_alpha = (1 - m_g) * m_alpha;
	}
	m_mlxCnpAlpha = false;
	m_mlxUpdateAlpha = Simulator::Schedule(NanoSeconds(m_flow.minRttNs - 1000), &RdmaQueuePair::UpdateMlxAlpha, this);
}

void
RdmaQueuePair::IncreaseMlxRate(){
	Simulator::Cancel(m_mlxIncreaseRate);
	if(m_mlxTimeStage > 0)
		m_mlxTargetRate = std::min(m_maxRate, m_mlxTargetRate + m_increase);
	m_currentRate = (m_mlxTargetRate + m_currentRate) * 0.5;
	m_mlxTimeStage += 1;
	m_mlxIncreaseRate = Simulator::Schedule(NanoSeconds(m_flow.minRttNs * 2), &RdmaQueuePair::IncreaseMlxRate, this);
}

void 
RdmaQueuePair::UpdateHpccRate(std::vector<IntHeader> newHeaders, bool fullUpdate){
	double Util = 0;
	uint64_t dt = 0;
	for(uint32_t i = 0;i < newHeaders.size();++i){
		auto& newHeader = newHeaders[i];
		auto& oldHeader = m_hpccHeaders[i];

		uint64_t tau = newHeader.GetTimeDelta(oldHeader);
		double duration = tau * 1e-9; // in seconds
		uint64_t bytes = newHeader.GetBytesDelta(oldHeader);
		double txRate = bytes * 8.0 / duration; // in bps
		double util = txRate / newHeader.GetRate().GetBitRate() +
			std::min(newHeader.GetQueueLen(), oldHeader.GetQueueLen()) / (m_flow.minRttNs * 1e-9) / m_maxRate.GetBitRate();
		
		if(util > Util){
			Util = util;
			dt = tau;
		}
		m_hpccHeaders[i] = newHeader;
	}

	DataRate newRate;
	int32_t newIncStage;

	dt = std::min(dt, m_flow.minRttNs);

	// std::cout << m_hpccUtil << " " << Util << " " << std::endl;
	m_hpccUtil = m_hpccUtil * (1.0 - dt / (double)m_flow.minRttNs) + Util * (dt / (double)m_flow.minRttNs);
	double maxC = m_hpccUtil / 0.95;

	if(maxC >= 1 || m_hpccIncStage >= 4){
		newRate = m_hpccPrevRate * (1.0 / maxC) + m_increase;
		newIncStage = 0;
	}
	else{
		newRate = m_hpccPrevRate + m_increase;
		newIncStage = m_hpccIncStage + 1;
	}

	newRate = std::min(newRate, m_maxRate);
	newRate = std::max(newRate, m_minRate);

	m_currentRate = newRate;

	if(fullUpdate){
		m_hpccPrevRate = newRate;
		m_hpccIncStage = newIncStage;
		m_hpccLastSeq = m_bytesSent + 1;
	}
}

} // namespace ns3