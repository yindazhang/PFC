#ifndef TOPOLOGY_H
#define TOPOLOGY_H

#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

std::string flowFile;
std::string logFile;

uint32_t ccVersion = 0;
uint32_t pfcVersion = 0;

// Fat-tree
std::vector<Ptr<Node>> servers;
std::vector<Ptr<PointToPointNetDevice>> nics;
std::vector<Ptr<SwitchNode>> tors;
std::vector<Ptr<SwitchNode>> aggs;
std::vector<Ptr<SwitchNode>> cores;

void BuildFatTreeRoute(
	uint32_t K, 
    uint32_t NUM_BLOCK ,
	uint32_t RATIO){

    // uint32_t numServer = K * K * NUM_BLOCK * RATIO;
    uint32_t numServerperRack = K * RATIO;

    uint32_t numTors = K * NUM_BLOCK;
    uint32_t numAggs = K * NUM_BLOCK;
    uint32_t numCores = K * K;

	for(uint32_t coreId = 0;coreId < numCores;++coreId){
		for(uint32_t serverId = 0;serverId < servers.size();++serverId){
			uint32_t blockId = serverId / K / K / RATIO;
			cores[coreId]->AddHostRouteTo(serverId, blockId + 1);
		}
	}

	for(uint32_t aggId = 0;aggId < numAggs;++aggId){
		for(uint32_t serverId = 0;serverId < servers.size();++serverId){
			uint32_t blockId = serverId / K / K / RATIO;
			if(blockId != aggId / K){
				for(uint32_t coreId = 1;coreId <= K;++coreId){
					aggs[aggId]->AddHostRouteTo(serverId, K + coreId);
				}
			}
			else{
				aggs[aggId]->AddHostRouteTo(serverId, (serverId / numServerperRack) % K + 1);
			}
		}
	}

	for(uint32_t torId = 0;torId < numTors;++torId){
		for(uint32_t serverId = 0;serverId < servers.size();++serverId){
			uint32_t rackId = serverId / numServerperRack;
			if(rackId != torId){
				for(uint32_t aggId = 1;aggId <= K;++aggId){
					tors[torId]->AddHostRouteTo(serverId, numServerperRack + aggId);
				}
			}
			else{
				tors[torId]->AddHostRouteTo(serverId, serverId % numServerperRack + 1);
			}
		}
	}
}

void BuildFatTree(
    std::string logFile,
    uint32_t K = 4, 
    uint32_t NUM_BLOCK = 5,
	uint32_t RATIO = 4){

	uint32_t numServer = K * K * NUM_BLOCK * RATIO;
    uint32_t numServerperRack = K * RATIO;

    uint32_t numTors = K * NUM_BLOCK;
    uint32_t numAggs = K * NUM_BLOCK;
    uint32_t numCores = K * K;

	servers.resize(numServer);
	tors.resize(numTors);
	aggs.resize(numAggs);
	cores.resize(numCores);

	for(uint32_t i = 0;i < numServer;++i){
		servers[i] = CreateObject<Node>();
	}
	for(uint32_t i = 0;i < numTors;++i){
		tors[i] = CreateObject<SwitchNode>(); 
		tors[i]->SetECMPHash(1);
		tors[i]->SetId(2000 + i);
        tors[i]->SetPFC(pfcVersion);
		tors[i]->SetCC(ccVersion);
	}
	for(uint32_t i = 0;i < numAggs;++i){
		aggs[i] = CreateObject<SwitchNode>();
		aggs[i]->SetECMPHash(2);
		aggs[i]->SetId(3000 + i);
		aggs[i]->SetPFC(pfcVersion);
		aggs[i]->SetCC(ccVersion);
	}
	for(uint32_t i = 0;i < numCores;++i){
		cores[i] = CreateObject<SwitchNode>();
		cores[i]->SetECMPHash(3);
		cores[i]->SetId(4000 + i);
		cores[i]->SetPFC(pfcVersion);
		cores[i]->SetCC(ccVersion);
	}

	InternetStackHelper internet;
    internet.InstallAll();

	// Initilize link
	PointToPointHelper linkServerSwitch;
	linkServerSwitch.SetDeviceAttribute("DataRate", StringValue("100Gbps"));
	linkServerSwitch.SetChannelAttribute("Delay", StringValue("1us"));

	PointToPointHelper linkSwitchSwitch;
	linkSwitchSwitch.SetDeviceAttribute("DataRate", StringValue("400Gbps"));
	linkSwitchSwitch.SetChannelAttribute("Delay", StringValue("1us"));

	Ipv4AddressHelper ipv4;
	for(uint32_t torId = 0;torId < numTors;++torId){
		for(uint32_t j = 0;j < numServerperRack;++j){
			uint32_t serverId = torId * numServerperRack + j;
			NetDeviceContainer ndc = linkServerSwitch.Install(servers[serverId], tors[torId]);
			auto nic = DynamicCast<PointToPointNetDevice>(ndc.Get(0));
			nic->SetId(serverId);
			nic->SetCC(ccVersion);
			nic->SetPFC(pfcVersion);
			nic->SetDeviceType(PointToPointNetDevice::NetDeviceType::SERVER);
			nics.push_back(nic);
		}
	}
	
	for(uint32_t blockId = 0;blockId < NUM_BLOCK;++blockId){
		for(uint32_t j = 0;j < K;++j){
			for(uint32_t k = 0;k < K;++k){
                uint32_t torId = blockId * K + j;
                uint32_t aggId = blockId * K + k;
				linkSwitchSwitch.Install(tors[torId], aggs[aggId]);
			}
		}
	}

	for(uint32_t blockId = 0;blockId < NUM_BLOCK;++blockId){
		for(uint32_t j = 0;j < K;++j){
			for(uint32_t k = 0;k < K;++k){
                uint32_t aggId = blockId * K + j;
                uint32_t coreId = j * K + k;
				linkSwitchSwitch.Install(aggs[aggId], cores[coreId]);
			}
		}
	}

	BuildFatTreeRoute(K, NUM_BLOCK, RATIO);
}

#endif 