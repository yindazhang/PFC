#include "ai-workload.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("PFC");

int
main(int argc, char* argv[])
{
    flowFile = "test";

    // Congestion control is NSCC (see RdmaSender); tune it with --ns3::RdmaSender::<Attribute>=...
    uint32_t mtuShift = 12;
    double stopTime = 10.0;
    bool spray = true;
    bool ecnIgnoreOnLb = false;
    bool evOnLb = true;
    bool evOnLoss = true;
    int ecnMark = -1; // -1: on unless srcRoute
    bool lbMark = true; // only SRv6 senders read it
    AiWorkloadConfig work;

    CommandLine cmd(__FILE__);
    // Workload
    cmd.AddValue("workload", "training | allreduce | alltoall | permutation | trace (replay --flow)",
                 work.type);
    cmd.AddValue("flow", "trace workload: the flow file trace/<flow>.tr", flowFile);
    cmd.AddValue("iters", "iterations (training / allreduce / alltoall)", work.iterations);
    cmd.AddValue("bytes", "allreduce / alltoall / permutation: bytes per rank", work.bytes);
    cmd.AddValue("pp", "training: pipeline stages (data parallel = servers / pp)", work.pp);
    cmd.AddValue("mb", "training: micro-batches per iteration", work.microbatches);
    cmd.AddValue("act", "training: activation / gradient bytes per micro-batch between stages",
                 work.activationBytes);
    cmd.AddValue("grad", "training: gradient bytes per rank, all-reduced per stage", work.gradientBytes);
    cmd.AddValue("fwdUs", "training: forward compute per micro-batch (us)", work.forwardUs);
    cmd.AddValue("bwdUs", "training: backward compute per micro-batch (us)", work.backwardUs);
    cmd.AddValue("placement", "rank -> server: packed | strided | random", work.placement);
    cmd.AddValue("groupSize",
                 "allreduce / alltoall / permutation: ranks per collective, all groups at once "
                 "(0: all ranks)",
                 work.groupSize);
    cmd.AddValue("channels", "ring all-reduce: independent rings (odd ones reversed)", work.ring.channels);
    cmd.AddValue("sliceBytes", "ring all-reduce: max slice per ring step", work.ring.sliceBytes);
    cmd.AddValue("slots", "ring all-reduce: slices in flight per rank and channel", work.ring.slots);
    // Network
    cmd.AddValue("pfc", "the version of PFC. 0 : no PFC, 1 : PFC", pfcVersion);
    cmd.AddValue("trim", "trim (instead of drop) packets on buffer overflow", trimEnabled);
    cmd.AddValue("egressAdmission",
                 "without PFC: shared-buffer admission per egress queue (0: per ingress port)",
                 egressAdmission);
    cmd.AddValue("mtuShift", "log2 of the MTU payload bytes: 8..12 (256..4096), by default 12", mtuShift);
    cmd.AddValue("planes", "topology: number of planes (NICs per server)", topo.numPlanes);
    cmd.AddValue("servers", "topology: number of servers (one GPU each), a multiple of t0",
                 topo.numServers);
    cmd.AddValue("t0", "topology: T0 switches per plane (at most 256)", topo.numT0PerPlane);
    cmd.AddValue("t1",
                 "topology: T1 switches per plane; servers/t0 is non-blocking, more is "
                 "undersubscribed, fewer oversubscribed",
                 topo.numT1PerPlane);
    cmd.AddValue("spray", "per-packet spraying (new flow label per packet); 0: per-flow ECMP", spray);
    cmd.AddValue("srcRoute", "MRC SRv6 source routing + EV management (overrides spray)", sourceRouting);
    cmd.AddValue("ecnMark",
                 "switches set the ECN bit (congestion control); -1: on unless srcRoute", ecnMark);
    cmd.AddValue("ecnKmin", "RED Kmin (KB) for a 100 Gbps port, scaled with the rate", ecnKminKB);
    cmd.AddValue("ecnKmax", "RED Kmax (KB) for a 100 Gbps port; 0: 4 x ecnKmin", ecnKmaxKB);
    cmd.AddValue("ecnPmax", "RED marking probability at Kmax", ecnPmax);
    cmd.AddValue("lbThreshold",
                 "load-balancing bit: mark every packet while the egress queue is above this "
                 "(KB for a 100 Gbps port, scaled with the rate)",
                 lbThresholdKB);
    cmd.AddValue("lbMode",
                 "load-balancing bit above lbThreshold: step (every packet), flow (connections "
                 "with hash(src, dst, switch, port) < p) or packet (each packet with probability "
                 "p), p = (queue - lbThreshold) / port buffer",
                 lbMode);
    cmd.AddValue("evOnLb", "srcRoute: the load-balancing mark switches paths", evOnLb);
    cmd.AddValue("evOnLoss", "srcRoute: a trim NACK or an RTO switches paths", evOnLoss);
    cmd.AddValue("ecnIgnoreOnLb",
                 "congestion control ignores the ECN of a packet that also carried the "
                 "load-balancing mark",
                 ecnIgnoreOnLb);
    cmd.AddValue("lbMark",
                 "switches set the load-balancing bit (MRC path switching; only srcRoute senders "
                 "read it; never on the last hop)",
                 lbMark);
    cmd.AddValue("ecnDequeue", "mark ECN at dequeue (0: at enqueue)", ecnAtDequeue);
    cmd.AddValue("degradeLinks", "number of random T0-T1 links running at degradeRate", degradeLinks);
    cmd.AddValue("degradeRate", "rate of a degraded T0-T1 link", degradeRate);
    cmd.AddValue("rate", "link rate of every plane", topo.linkRate);
    cmd.AddValue("delay", "delay of every link", topo.linkDelay);
    cmd.AddValue("stop", "simulated-time limit (s), a safeguard if messages never complete", stopTime);
    cmd.Parse(argc, argv);

    if (mtuShift < 8 || mtuShift > 12)
    {
        std::cerr << "Unsupported mtuShift " << mtuShift << ". Use 8..12 (MTU 256..4096)." << std::endl;
        return 1;
    }
    Config::SetDefault("ns3::RdmaSender::MtuShift", UintegerValue(mtuShift));
    Config::SetDefault("ns3::RdmaSender::PacketSpraying", BooleanValue(spray));
    Config::SetDefault("ns3::RdmaSender::SourceRouting", BooleanValue(sourceRouting));
    Config::SetDefault("ns3::RdmaSender::EcnIgnoreOnLb", BooleanValue(ecnIgnoreOnLb));
    Config::SetDefault("ns3::RdmaSender::EvOnLb", BooleanValue(evOnLb));
    Config::SetDefault("ns3::RdmaSender::EvOnLoss", BooleanValue(evOnLoss));
    // SRv6: path switching uses the load-balancing bit, and switches mark it instead of ECN
    ecnMarking = ecnMark < 0 ? !sourceRouting : ecnMark != 0;
    lbMarking = lbMark;

    std::string name = work.type == "trace" ? flowFile + "s" : work.type;
    std::filesystem::create_directories("logs");
    logFile = "logs/" + name + "_P" + std::to_string(topo.numPlanes) + "_S" +
              std::to_string(topo.numServers) + "_T" + std::to_string(topo.numT0PerPlane) + "x" +
              std::to_string(topo.numT1PerPlane) + "_PFC" +
              std::to_string(pfcVersion) + "_TRIM" + std::to_string(trimEnabled) +
              (sourceRouting ? std::string("_SRV6") : "_SPRAY" + std::to_string(spray)) + "_ECN" +
              std::to_string(ecnMarking) + std::to_string(lbMarking) + (ecnAtDequeue ? "D" : "E") +
              "_DEG" +
              std::to_string(degradeLinks) + "_MTU" + std::to_string(1u << mtuShift);

    BuildMultiPlane();
    BuildConnections();
    if (!OpenFctLog())
    {
        return 1;
    }

    AiWorkload workload;
    if (work.type == "trace")
    {
        if (!ScheduleFlow())
        {
            return 1;
        }
    }
    else
    {
        BuildWorkload(workload, work, topo.numServers);
        std::cout << "Workload " << work.type << ": " << workload.GetNTasks() << " tasks" << std::endl;
        // Start at 2 s like the traces (FCT logs keep their time base)
        Simulator::Schedule(Seconds(2), &AiWorkload::Start, &workload);
    }

    std::cout << "Start simulation" << std::endl;
    auto start = std::chrono::system_clock::now();

    // Ends when every message completes (idle QPs keep no events), or at the limit
    Simulator::Stop(Seconds(stopTime));
    Simulator::Run();
    fclose(logFilePtr);

    const auto& st = RdmaSender::GetGlobalStats();
    std::cout << "Senders: " << st.packets << " data packets, " << st.retransmissions
              << " retransmissions, " << st.rtos << " RTOs, " << st.reorderLosses
              << " reorder losses (" << st.spuriousDetections << " spurious seen), " << st.trimNacks << " trim NACKs, " << st.evSwaps
              << " EV swaps (" << st.evRejoins << " rejoined)" << std::endl;
    uint64_t drops = 0;
    uint64_t trims = 0;
    uint64_t marks = 0;
    uint64_t lbMarks = 0;
    int32_t maxQueue = 0;
    for (auto* tier : {&t0s, &t1s})
    {
        for (auto& plane : *tier)
        {
            for (auto& sw : plane)
            {
                drops += sw->GetDropCount();
                trims += sw->GetTrimCount();
                marks += sw->GetEcnMarkCount();
                lbMarks += sw->GetLbMarkCount();
                maxQueue = std::max(maxQueue, sw->GetMaxEgressUsed());
            }
        }
    }
    std::cout << "Switches: " << drops << " drops, " << trims << " trims, " << marks
              << " ECN marks, " << lbMarks << " LB marks, max queue " << maxQueue / 1000 << " KB. Fabric load:" << std::endl;
    PrintFabricLoad(std::cout);

    std::chrono::duration<double> diff = std::chrono::system_clock::now() - start;
    std::cout << "Messages: " << numMessages << " on " << numQps << " QPs, FCT log: " << logFile
              << ".fct" << std::endl;
    if (work.type != "trace")
    {
        std::cout << "Tasks done: " << workload.GetNDone() << "/" << workload.GetNTasks()
                  << ", phases (" << logFile << ".jct):" << std::endl;
        workload.Report(std::cout);
        std::ofstream jct(logFile + ".jct");
        workload.Report(jct);
        workload.ReportCollectives(std::cout);
    }
    std::cout << "Used time: " << diff.count() << "s." << std::endl;
    Simulator::Destroy();
    return 0;
}
