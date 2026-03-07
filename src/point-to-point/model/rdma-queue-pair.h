#ifndef RDMA_QUEUE_PAIR_H
#define RDMA_QUEUE_PAIR_H

#include "ns3/node.h"
#include "ns3/socket.h"
#include "ns3/ipv4-header.h"

#include "bth-header.h"
#include "hpcc-header.h"
#include "point-to-point-net-device.h"

namespace ns3
{

struct FlowInfo
{
    uint32_t id;
    uint32_t src;
    uint32_t dst;
    uint32_t size;
    uint64_t startTime;
    uint64_t endTime;
	uint64_t minRttNs;

    FlowInfo(uint32_t _id = 0, uint32_t _src = 0, uint32_t _dst = 0, uint32_t _size = 0, uint64_t _startTime = 0, uint64_t _endTime = 0, uint64_t _minRttNs = 0)
        : id(_id), src(_src), dst(_dst), size(_size), startTime(_startTime), endTime(_endTime), minRttNs(_minRttNs)
    {
    }
};

class PointToPointNetDevice;

class RdmaQueuePair : public Object
{
public:
	static TypeId GetTypeId();

    RdmaQueuePair(FlowInfo flow, Ptr<PointToPointNetDevice> device = nullptr, FILE* logFilePtr = nullptr, uint32_t ccVersion = 0, uint32_t pfcVersion = 0);

	uint32_t GetId() const { return m_flow.id; }

	bool IsSendCompleted() const;

	bool ProcessACK(BthHeader& bth_header, HpccHeader& hpcc_header);

	Ptr<Packet> GenerateNextPacket();

	int64_t GetNextSendTime();

	int64_t GetTimeOut();

	void TimeOutReset();

	static const uint8_t m_dataPriority{2};
	static const uint8_t m_ackPriority{2};

private:
	uint16_t m_port{0};

	uint32_t m_sendSize{4000};
	uint32_t m_bytesSent{0};
	uint32_t m_bytesAcked{0};

	DataRate m_maxRate;
	DataRate m_minRate;
	DataRate m_currentRate;
	DataRate m_increase;

	int64_t m_lastSendTime{0};
	int64_t m_lastGenerateTime{0};

	FlowInfo m_flow;

	Ptr<PointToPointNetDevice> m_device;
	FILE* m_logFile;

	void WriteFCT();

	// Congestion Control
	uint32_t m_ccVersion{0};
	uint32_t m_pfcVersion{0};
	int64_t m_prevCnpTime{0};

	// Congestion control variables
	double m_alpha{1.0};
	double m_g{1.0 / 256.0};

	// New DCTCP
	uint64_t m_win;

	void ProcessNewDctcpACK(uint32_t ackedBytes, bool cnp);

	// DCTCP variables
	bool m_dctcpCongested{false};

	uint32_t m_dctcpEcnCount{0};
	uint32_t m_dctcpLastSeq{0};
	uint32_t m_dctcpLastEcn{0};
	uint32_t m_dctcpAlphaSize{0};

	void ProcessDctcpACK(uint32_t ackedBytes, bool cnp);

	// MLX variables
	bool m_mlxCnpAlpha{false};

	int32_t m_mlxTimeStage{0};

	DataRate m_mlxTargetRate;

	EventId m_mlxUpdateAlpha;
	EventId m_mlxIncreaseRate;

	void DecreaseMlxRate();

	void UpdateMlxAlpha();
	void IncreaseMlxRate();

	// HPCC variables
	uint32_t m_hpccLastSeq{0};
	uint32_t m_hpccIncStage{0};
	double m_hpccUtil{0.0};
	DataRate m_hpccPrevRate{0};

	std::vector<IntHeader> m_hpccHeaders;

	void UpdateHpccRate(std::vector<IntHeader> newHeaders, bool fullUpdate);
};

} // namespace ns3

#endif /* RDMA_QUEUE_PAIR */