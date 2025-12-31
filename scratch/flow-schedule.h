#ifndef FLOW_SCHEDULER_H
#define FLOW_SCHEDULER_H

#include <stdio.h>

#include "topology.h"

using namespace ns3;

FILE* logFilePtr = nullptr;
FILE* flowFilePtr = nullptr;

FlowInfo currentFlow;

void ReadLine();

void SetFlow(){
    Ptr<PointToPointNetDevice> nic = nics[currentFlow.src];
    nic->SetFlow(currentFlow, logFilePtr, ccVersion);
    ReadLine();
}

void ReadLine(){
    if(fscanf(flowFilePtr, "%u %u %u %lu", &currentFlow.src, &currentFlow.dst, &currentFlow.size, &currentFlow.startTime) != EOF){
        currentFlow.id += 1;
        if(Simulator::Now() != NanoSeconds(currentFlow.startTime)){
            Simulator::Schedule(NanoSeconds(currentFlow.startTime) - Simulator::Now(), &SetFlow);
        }
        else{
            SetFlow();
        }
    }
}

void ScheduleFlow(){
    logFilePtr = fopen((logFile + ".fct").c_str(), "w");
    flowFilePtr = fopen(("trace/" + flowFile + ".tr").c_str(), "r");

    ReadLine();
}

#endif /* FLOW_SCHEDULER_H */