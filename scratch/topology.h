#ifndef TOPOLOGY_H
#define TOPOLOGY_H

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
#include <sstream>
#include <vector>

using namespace ns3;

std::string flowFile;
std::string logFile;

uint32_t pfcVersion = 0; //!< 1: PFC enabled on the switches
bool trimEnabled = false;
bool sourceRouting = false; //!< MRC SRv6 source routing + EV management at the senders
bool ecnAtDequeue = true;   //!< Mark ECN at dequeue (false: at enqueue)
bool egressAdmission = true; //!< Without PFC: dynamic threshold per egress queue (false: per ingress port)
bool ecnMarking = true;      //!< Switches set the ECN bit (congestion control) on congestion
bool lastHopEcn = false;     //!< ECN also on the last hop (T0 -> server), which every path shares
uint32_t ecnKminKB = 100;    //!< RED Kmin for a 100 Gbps port (scaled with the rate), as FASTFLOW
uint32_t ecnKmaxKB = 0;      //!< RED Kmax for a 100 Gbps port; 0: 4 x Kmin
double ecnPmax = 1.0;        //!< RED marking probability at Kmax, as FASTFLOW
uint32_t degradeLinks = 0;          //!< Random T0-T1 links running below the plane rate
std::string degradeRate = "50Gbps"; //!< Rate of a degraded link (both directions)
uint32_t intVersion = 0;            //!< INT added by the T1 (fattree: aggregation and core) switches; 0: off
std::string intLevels = "10+45+80"; //!< Telemetry levels, % of the port buffer, separated by + / ,
double intHysteresis = 5;           //!< Telemetry: back down a level this many % below its threshold
std::vector<double> intLevelFractions; //!< Parsed intLevels, as fractions (ParseIntLevels)
std::string intLeaves = "all";      //!< T0 switches that record snapshots (see IsIntLeaf)
std::string intAggs = "all";        //!< fattree: aggregation switches that record snapshots

/**
 * Multi-plane topology: every server has one NIC per plane and each plane is
 * an independent Clos:
 *   - leafspine (default): two tiers, T0 leaf and T1 spine, full mesh;
 *   - fattree: three tiers. T0 = ToR, T1 = aggregation, T2 = core. Pods of
 *     K ToRs and K aggregation switches (full mesh inside the pod); the
 *     aggregation switch a of every pod connects to the K cores of group a
 *     (K * K cores). A ToR has K uplinks, so servers / T0 / K : 1
 *     oversubscription (4:1 for the defaults: 16 servers per ToR, K = 4).
 */
struct MultiPlaneConfig
{
    std::string type = "leafspine"; //!< leafspine | fattree
    uint32_t numPlanes = 0;         //!< 0: the topology's default (leafspine 4, fattree 1)
    uint32_t numServers = 256;
    uint32_t numT0PerPlane = 16;
    uint32_t numT1PerPlane = 16; //!< leafspine: non-blocking when equal to the servers per T0
    uint32_t k = 4;              //!< fattree: ToRs and aggregation switches per pod, cores per group
    std::string linkRate = "100Gbps"; //!< Per plane, all links
    std::string linkDelay = "1us";
};

MultiPlaneConfig topo;

std::vector<Ptr<Node>> servers;
std::vector<std::vector<Ptr<PointToPointNetDevice>>> nics; //!< [server][plane]
std::vector<std::vector<Ptr<SwitchNode>>> t0s;              //!< [plane][t0]
std::vector<std::vector<Ptr<SwitchNode>>> t1s;              //!< [plane][t1] (fattree: aggregation)
std::vector<std::vector<Ptr<SwitchNode>>> t2s;              //!< [plane][core] (fattree only)

uint32_t serversPerT0 = 0;

struct FabricLink
{
    uint32_t plane;
    uint32_t t0; //!< T0 (fattree tier 1: aggregation switch)
    uint32_t t1; //!< T1; fattree: the ToR's uplink index (tier 0) or the core (tier 1)
    NetDeviceContainer devices;
    uint32_t tier{0}; //!< fattree: 0 ToR - aggregation, 1 aggregation - core
};

std::vector<FabricLink> fabricLinks; //!< All switch-to-switch links

/** Parse intLevels into intLevelFractions; false (with a message) if invalid. */
bool
ParseIntLevels()
{
    std::string list = intLevels;
    std::replace(list.begin(), list.end(), '+', ',');
    std::replace(list.begin(), list.end(), '/', ',');
    std::stringstream ss(list);
    std::string item;
    intLevelFractions.clear();
    while (std::getline(ss, item, ','))
    {
        if (!item.empty())
        {
            intLevelFractions.push_back(std::stod(item) / 100);
        }
    }
    bool ok = !intLevelFractions.empty() && intLevelFractions.size() <= 9 && intHysteresis > 0 &&
              intHysteresis / 100 < intLevelFractions[0];
    for (uint32_t i = 1; ok && i < intLevelFractions.size(); i++)
    {
        ok = intLevelFractions[i] > intLevelFractions[i - 1];
    }
    if (!ok)
    {
        std::cerr << "--intLevels needs 1 to 9 increasing % of the port buffer, all above "
                     "--intHysteresis > 0 (got "
                  << intLevels << ", " << intHysteresis << ")" << std::endl;
    }
    return ok;
}

bool
IsFatTree()
{
    return topo.type == "fattree";
}

/** Fill in the topology's defaults; false (with a message) if the options are invalid. */
bool
ResolveTopology()
{
    if (topo.type != "leafspine" && topo.type != "fattree")
    {
        std::cerr << "Unknown --topo=" << topo.type << ". Use leafspine or fattree." << std::endl;
        return false;
    }
    if (topo.numPlanes == 0)
    {
        topo.numPlanes = IsFatTree() ? 1 : 4;
    }
    if (IsFatTree() && (topo.k == 0 || topo.numT0PerPlane % topo.k != 0))
    {
        std::cerr << "fattree: --t0 must be a multiple of --k" << std::endl;
        return false;
    }
    return true;
}

/** fattree: pod of ToR \p t0. */
uint32_t
PodOf(uint32_t t0)
{
    return t0 / topo.k;
}

/** SRv6 locator of a plane. */
uint32_t
PlaneLocator(uint32_t plane)
{
    return 0xfc000000 | plane;
}

/** uSID of a switch inside its plane (0 is reserved for End-of-Carrier). */
uint16_t
T0Usid(uint32_t t0)
{
    return 0x0100 + t0;
}

uint16_t
T1Usid(uint32_t t1)
{
    return 0x0200 + t1;
}

/** fattree: uSID of core \p core. */
uint16_t
T2Usid(uint32_t core)
{
    return 0x8000 + core;
}

/** Same-T0 flows cross 2 links each way, others 4 (fattree: same pod 4, others 6). */
uint64_t
BaseRttNs(uint32_t src, uint32_t dst)
{
    Time delay(topo.linkDelay);
    uint32_t t0Src = src / serversPerT0;
    uint32_t t0Dst = dst / serversPerT0;
    uint32_t hops = t0Src == t0Dst ? 2 : (IsFatTree() && PodOf(t0Src) != PodOf(t0Dst)) ? 6 : 4;
    return 2 * hops * delay.GetNanoSeconds();
}

/**
 * SRv6 paths (one per EV) from src to dst on every plane: [T0] within a T0,
 * otherwise [T0 src, T1 j, T0 dst] for every T1 j of the plane. fattree:
 * within a pod [ToR src, Agg a, ToR dst] for each of its K aggregation
 * switches; across pods [ToR src, Agg a, Core c, Agg a, ToR dst] for each
 * aggregation index a and core c of group a (K * K paths).
 */
std::vector<std::pair<uint32_t, Srv6Sid>>
SourceRoutes(uint32_t src, uint32_t dst)
{
    std::vector<std::pair<uint32_t, Srv6Sid>> routes;
    uint32_t t0Src = src / serversPerT0;
    uint32_t t0Dst = dst / serversPerT0;
    for (uint32_t p = 0; p < topo.numPlanes; p++)
    {
        if (t0Src == t0Dst)
        {
            routes.emplace_back(p, Srv6Sid(PlaneLocator(p), {T0Usid(t0Src)}));
            continue;
        }
        if (IsFatTree())
        {
            uint32_t k = topo.k;
            uint32_t podSrc = PodOf(t0Src);
            uint32_t podDst = PodOf(t0Dst);
            for (uint32_t a = 0; a < k; a++)
            {
                if (podSrc == podDst)
                {
                    routes.emplace_back(p,
                                        Srv6Sid(PlaneLocator(p),
                                                {T0Usid(t0Src), T1Usid(podSrc * k + a), T0Usid(t0Dst)}));
                    continue;
                }
                for (uint32_t c = 0; c < k; c++)
                {
                    routes.emplace_back(p,
                                        Srv6Sid(PlaneLocator(p),
                                                {T0Usid(t0Src),
                                                 T1Usid(podSrc * k + a),
                                                 T2Usid(a * k + c),
                                                 T1Usid(podDst * k + a),
                                                 T0Usid(t0Dst)}));
                }
            }
            continue;
        }
        for (uint32_t j = 0; j < topo.numT1PerPlane; j++)
        {
            routes.emplace_back(
                p,
                Srv6Sid(PlaneLocator(p), {T0Usid(t0Src), T1Usid(j), T0Usid(t0Dst)}));
        }
    }
    return routes;
}

/**
 * Run \p count random T0-T1 links (both directions) at \p rate, making the
 * fabric asymmetric. The choice depends only on the topology and \p seed, so
 * every configuration degrades the same links.
 */
void
DegradeRandomLinks(uint32_t count, DataRate rate, uint32_t seed = 1)
{
    NS_ABORT_MSG_IF(count > fabricLinks.size(), "degradeLinks exceeds the number of fabric links");
    std::vector<uint32_t> order(fabricLinks.size());
    for (uint32_t i = 0; i < order.size(); i++)
    {
        order[i] = i;
    }
    std::mt19937 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);
    for (uint32_t k = 0; k < count; k++)
    {
        const FabricLink& link = fabricLinks[order[k]];
        for (uint32_t d = 0; d < link.devices.GetN(); d++)
        {
            DynamicCast<PointToPointNetDevice>(link.devices.Get(d))->SetDataRate(rate);
        }
        if (link.tier == 1)
        {
            std::cout << "Degraded link to " << rate << ": plane " << link.plane << " Agg "
                      << link.t0 << " - Core " << link.t1 << std::endl;
        }
        else
        {
            // fattree: T1 j is the ToR's uplink j (aggregation switch j of its pod)
            std::cout << "Degraded link to " << rate << ": plane " << link.plane << " T0 "
                      << link.t0 << " - T1 " << link.t1 << std::endl;
        }
    }
}

/**
 * Whether T0 \p t0 of plane \p plane records INT snapshots. \p spec is "all",
 * "none" (or empty), or a list, separated by ',' or '+' (commands/run.sh
 * sweeps commas), of "i" (T0 i in every plane) and "p:i" (T0 i of plane p).
 */
bool
IsIntLeaf(const std::string& spec, uint32_t plane, uint32_t t0)
{
    if (spec == "all")
    {
        return true;
    }
    std::string list = spec;
    std::replace(list.begin(), list.end(), '+', ',');
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        if (item.empty() || item == "none")
        {
            continue;
        }
        size_t colon = item.find(':');
        if (colon == std::string::npos ? std::stoul(item) == t0
                                       : std::stoul(item.substr(0, colon)) == plane &&
                                             std::stoul(item.substr(colon + 1)) == t0)
        {
            return true;
        }
    }
    return false;
}

FILE* intLogPtr = nullptr; //!< Snapshot log, written as snapshots are taken
uint64_t intLogRows = 0;

/** Open the snapshot log (CSV, see WriteIntSnapshot). \return false on error. */
bool
OpenIntLog(const std::string& path)
{
    intLogPtr = fopen(path.c_str(), "w");
    if (intLogPtr == nullptr)
    {
        return false;
    }
    fprintf(intLogPtr, "time_ns,plane,switch,index,dir,port,level,queue_bytes,levels,bytes\n");
    return true;
}

/**
 * One row per level change of a monitored port of switch \p index (\p kind
 * "T0" or, fattree, "Agg") in \p plane, in one direction: out (its own egress
 * queue; bytes sent) or in (the upstream queue from INT; bytes received).
 * Port j is uplink j: to T1 j (leaf-spine), to aggregation switch j of the
 * pod (fattree ToR) or to core j of the group (fattree aggregation). levels
 * has one digit per port (0 = below the first threshold); bytes is
 * ';'-separated in port order, cumulative (LP only).
 */
void
WriteIntSnapshot(std::string kind, uint32_t plane, uint32_t index, const SwitchNode::IntSnapshot& s)
{
    if (intLogPtr == nullptr)
    {
        return;
    }
    std::string levels;
    std::string bytes;
    for (uint32_t j = 0; j < s.levels.size(); j++)
    {
        levels += char('0' + s.levels[j]);
        bytes += (j ? ";" : "") + std::to_string(s.bytes[j]);
    }
    fprintf(intLogPtr, "%lld,%u,%s,%u,%s,%u,%u,%u,%s,%s\n", (long long)s.time.GetNanoSeconds(), plane,
            kind.c_str(), index, s.direction == SwitchNode::MONITOR_OUT ? "out" : "in", s.port,
            unsigned(s.level), s.queueLength, levels.c_str(), bytes.c_str());
    intLogRows++;
}

/**
 * Low-priority load on the T0 -> T1 uplinks and T1 -> T0 downlinks: mean and
 * max bytes per link and max / mean, per plane and overall.
 */
void
PrintFabricLoad(std::ostream& os)
{
    auto summary = [&os](const std::string& name, const std::vector<uint64_t>& bytes) {
        if (bytes.empty())
        {
            return;
        }
        uint64_t max = 0;
        double sum = 0;
        for (uint64_t b : bytes)
        {
            max = std::max(max, b);
            sum += b;
        }
        double mean = sum / bytes.size();
        os << "  " << name << ": mean " << mean / 1e6 << " MB, max " << max / 1e6
           << " MB, max/mean " << (mean > 0 ? max / mean : 0) << std::endl;
    };
    DataRate planeRate(topo.linkRate);
    if (IsFatTree())
    {
        // Ports: ToR serversPerT0 + a -> aggregation a of its pod; aggregation
        // 0..K-1 -> the pod's ToRs, K..2K-1 -> its cores; core q -> pod q
        uint32_t k = topo.k;
        std::vector<uint64_t> torUp, aggDown, aggUp, coreDown, degraded, healthy;
        for (uint32_t p = 0; p < topo.numPlanes; p++)
        {
            for (auto& tor : t0s[p])
            {
                for (uint32_t a = 0; a < k; a++)
                {
                    uint64_t bytes = tor->GetTxBytesLp(serversPerT0 + a);
                    torUp.push_back(bytes);
                    auto dev = DynamicCast<PointToPointNetDevice>(tor->GetDevice(serversPerT0 + a));
                    (dev->GetDataRate() < planeRate ? degraded : healthy).push_back(bytes);
                }
            }
            for (auto& agg : t1s[p])
            {
                for (uint32_t i = 0; i < k; i++)
                {
                    aggDown.push_back(agg->GetTxBytesLp(i));
                    aggUp.push_back(agg->GetTxBytesLp(k + i));
                }
            }
            for (auto& core : t2s[p])
            {
                for (uint32_t q = 0; q < core->GetNDevices(); q++)
                {
                    coreDown.push_back(core->GetTxBytesLp(q));
                }
            }
        }
        summary("all ToR->Agg", torUp);
        summary("all Agg->ToR", aggDown);
        summary("all Agg->Core", aggUp);
        summary("all Core->Agg", coreDown);
        if (!degraded.empty())
        {
            summary("degraded ToR->Agg", degraded);
            summary("healthy ToR->Agg", healthy);
        }
        return;
    }
    std::vector<uint64_t> allUp;
    std::vector<uint64_t> allDown;
    std::vector<uint64_t> degradedUp; //!< Over degraded links only (below the plane rate)
    std::vector<uint64_t> healthyUp;
    for (uint32_t p = 0; p < topo.numPlanes; p++)
    {
        std::vector<uint64_t> up;
        std::vector<uint64_t> down;
        for (auto& t0 : t0s[p])
        {
            for (uint32_t j = 0; j < topo.numT1PerPlane; j++)
            {
                uint64_t bytes = t0->GetTxBytesLp(serversPerT0 + j);
                up.push_back(bytes);
                auto dev = DynamicCast<PointToPointNetDevice>(t0->GetDevice(serversPerT0 + j));
                (dev->GetDataRate() < planeRate ? degradedUp : healthyUp).push_back(bytes);
            }
        }
        for (auto& t1 : t1s[p])
        {
            for (uint32_t i = 0; i < topo.numT0PerPlane; i++)
            {
                down.push_back(t1->GetTxBytesLp(i));
            }
        }
        summary("plane " + std::to_string(p) + " T0->T1", up);
        summary("plane " + std::to_string(p) + " T1->T0", down);
        allUp.insert(allUp.end(), up.begin(), up.end());
        allDown.insert(allDown.end(), down.begin(), down.end());
    }
    summary("all T0->T1", allUp);
    summary("all T1->T0", allDown);
    if (!degradedUp.empty())
    {
        summary("degraded T0->T1", degradedUp);
        summary("healthy T0->T1", healthyUp);
    }
}

Ptr<SwitchNode>
CreateSwitch(uint32_t plane, uint16_t usid, uint32_t ecmpSeed)
{
    Ptr<SwitchNode> sw = CreateObject<SwitchNode>();
    sw->SetSwitchId(PlaneLocator(plane), usid);
    sw->SetEcmpSeed(ecmpSeed);
    sw->SetPfcEnabled(pfcVersion == 1);
    sw->SetEgressAdmission(egressAdmission);
    sw->SetEcnMarking(ecnMarking);
    sw->SetEcnThresholds(ecnKminKB * 1000, (ecnKmaxKB ? ecnKmaxKB : 4 * ecnKminKB) * 1000, ecnPmax);
    sw->SetTrimEnabled(trimEnabled);
    sw->SetEcnAtDequeue(ecnAtDequeue);
    return sw;
}

/** Servers <-> T0 links of plane \p p (shared by both topologies). */
void
ConnectServers(uint32_t p, PointToPointHelper& link)
{
    for (uint32_t s = 0; s < topo.numServers; s++)
    {
        Ptr<SwitchNode> t0 = t0s[p][s / serversPerT0];
        NetDeviceContainer ndc = link.Install(servers[s], t0);
        auto nic = DynamicCast<PointToPointNetDevice>(ndc.Get(0));
        nic->SetId(s);
        nic->SetDeviceType(PointToPointNetDevice::NetDeviceType::SERVER);
        nics[s][p] = nic;
        t0->AddRoute(RdmaSender::GetHostAddress(s), t0->GetNDevices() - 1);
        t0->SetEcnEnabled(t0->GetNDevices() - 1, lastHopEcn);
    }
}

/**
 * Telemetry: switch \p sw (\p kind, \p index in plane \p p) watches its
 * \p numUplinks uplinks, ports firstPort.., both out and in.
 */
void
SetIntMonitor(Ptr<SwitchNode> sw, std::string kind, uint32_t p, uint32_t index, uint32_t firstPort,
              uint32_t numUplinks)
{
    std::vector<uint32_t> uplinks;
    for (uint32_t j = 0; j < numUplinks; j++)
    {
        uplinks.push_back(firstPort + j);
    }
    sw->SetIntMonitor(uplinks, intLevelFractions, intHysteresis / 100);
    sw->SetIntSnapshotCallback(MakeBoundCallback(&WriteIntSnapshot, kind, p, index));
}

/** Telemetry: the chosen T0s of plane \p p watch their \p numUplinks uplinks (ports serversPerT0 + j). */
void
SetIntMonitors(uint32_t p, uint32_t numUplinks)
{
    for (uint32_t i = 0; intVersion != 0 && i < t0s[p].size(); i++)
    {
        if (IsIntLeaf(intLeaves, p, i))
        {
            SetIntMonitor(t0s[p][i], "T0", p, i, serversPerT0, numUplinks);
        }
    }
}

/**
 * fattree (see MultiPlaneConfig). Routing: a ToR sends traffic for other ToRs
 * over its K uplinks (ECMP); an aggregation switch sends it down to a ToR of
 * its pod, else up over its K cores (ECMP); a core sends it down to the
 * destination pod. SRv6: every switch routes the uSIDs of its neighbours.
 * INT is added by the aggregation and core switches: a ToR learns the queues of
 * the aggregation switches towards it, an aggregation switch those of its
 * cores. Telemetry: the chosen ToRs (--intLeaves) and aggregation switches
 * (--intAggs) watch their uplinks, out and in.
 */
void
BuildFatTree()
{
    const uint32_t P = topo.numPlanes;
    const uint32_t k = topo.k;
    const uint32_t tors = topo.numT0PerPlane;
    const uint32_t pods = tors / k;
    NS_ABORT_MSG_IF(P == 0 || tors == 0 || topo.numServers == 0, "planes, servers and t0 must be at least 1");
    NS_ABORT_MSG_IF(topo.numServers % tors != 0, "numServers must be a multiple of the ToRs per plane");
    NS_ABORT_MSG_IF(tors > uint32_t(T1Usid(0) - T0Usid(0)), "at most 256 ToRs per plane");
    NS_ABORT_MSG_IF(pods * k > uint32_t(T2Usid(0) - T1Usid(0)), "too many aggregation switches per plane");
    NS_ABORT_MSG_IF(k * k > uint32_t(0x10000 - T2Usid(0)), "too many core switches per plane");
    serversPerT0 = topo.numServers / tors;

    servers.resize(topo.numServers);
    for (auto& server : servers)
    {
        server = CreateObject<Node>();
    }
    nics.assign(topo.numServers, std::vector<Ptr<PointToPointNetDevice>>(P));
    t0s.assign(P, {});
    t1s.assign(P, {});
    t2s.assign(P, {});

    PointToPointHelper link;
    link.SetDeviceAttribute("DataRate", StringValue(topo.linkRate));
    link.SetChannelAttribute("Delay", StringValue(topo.linkDelay));

    for (uint32_t p = 0; p < P; p++)
    {
        uint32_t locator = PlaneLocator(p);
        for (uint32_t i = 0; i < tors; i++)
        {
            t0s[p].push_back(CreateSwitch(p, T0Usid(i), 1));
        }
        for (uint32_t g = 0; g < pods * k; g++)
        {
            t1s[p].push_back(CreateSwitch(p, T1Usid(g), 2));
            t1s[p].back()->SetIntVersion(intVersion);
        }
        for (uint32_t c = 0; c < k * k; c++)
        {
            t2s[p].push_back(CreateSwitch(p, T2Usid(c), 3));
            t2s[p].back()->SetIntVersion(intVersion); // INT for the aggregation switches below
        }

        ConnectServers(p, link);

        // ToR <-> aggregation, full mesh inside each pod
        for (uint32_t i = 0; i < tors; i++)
        {
            uint32_t pod = PodOf(i);
            for (uint32_t a = 0; a < k; a++)
            {
                uint32_t g = pod * k + a;
                Ptr<SwitchNode> tor = t0s[p][i];
                Ptr<SwitchNode> agg = t1s[p][g];
                fabricLinks.push_back({p, i, a, link.Install(tor, agg), 0});
                uint32_t torPort = tor->GetNDevices() - 1;
                uint32_t aggPort = agg->GetNDevices() - 1;
                tor->AddSidRoute(locator, T1Usid(g), torPort);
                agg->AddSidRoute(locator, T0Usid(i), aggPort);
                for (uint32_t s = 0; s < topo.numServers; s++)
                {
                    Ipv6Address dst = RdmaSender::GetHostAddress(s);
                    if (s / serversPerT0 == i)
                    {
                        agg->AddRoute(dst, aggPort); // down to the server's ToR
                    }
                    else
                    {
                        tor->AddRoute(dst, torPort); // ECMP over the K uplinks
                    }
                }
            }
        }

        // Aggregation a of every pod <-> the K cores of group a
        for (uint32_t pod = 0; pod < pods; pod++)
        {
            for (uint32_t a = 0; a < k; a++)
            {
                uint32_t g = pod * k + a;
                for (uint32_t c = a * k; c < (a + 1) * k; c++)
                {
                    Ptr<SwitchNode> agg = t1s[p][g];
                    Ptr<SwitchNode> core = t2s[p][c];
                    fabricLinks.push_back({p, g, c, link.Install(agg, core), 1});
                    uint32_t aggPort = agg->GetNDevices() - 1;
                    uint32_t corePort = core->GetNDevices() - 1;
                    agg->AddSidRoute(locator, T2Usid(c), aggPort);
                    core->AddSidRoute(locator, T1Usid(g), corePort);
                    for (uint32_t s = 0; s < topo.numServers; s++)
                    {
                        Ipv6Address dst = RdmaSender::GetHostAddress(s);
                        if (PodOf(s / serversPerT0) == pod)
                        {
                            core->AddRoute(dst, corePort); // down to the server's pod
                        }
                        else
                        {
                            agg->AddRoute(dst, aggPort); // ECMP over the K cores
                        }
                    }
                }
            }
        }

        SetIntMonitors(p, k);
        // Aggregation switches watch their K core uplinks (ports K.., after the K ToRs)
        for (uint32_t g = 0; intVersion != 0 && g < pods * k; g++)
        {
            if (IsIntLeaf(intAggs, p, g))
            {
                SetIntMonitor(t1s[p][g], "Agg", p, g, k, k);
            }
        }
    }

    std::cout << "Fat-tree topology: " << P << " planes x " << topo.linkRate << ", "
              << topo.numServers << " servers, K=" << k << ", " << pods << " pods, " << tors
              << " ToR + " << pods * k << " Agg + " << k * k << " Core per plane ("
              << serversPerT0 << " servers per ToR, oversubscription "
              << double(serversPerT0) / k << ":1 at the ToR)" << std::endl;

    if (degradeLinks > 0)
    {
        DegradeRandomLinks(degradeLinks, DataRate(degradeRate));
    }
}

void
BuildMultiPlane()
{
    if (IsFatTree())
    {
        BuildFatTree();
        return;
    }
    const uint32_t P = topo.numPlanes;
    const uint32_t t0PerPlane = topo.numT0PerPlane;
    const uint32_t t1PerPlane = topo.numT1PerPlane;
    NS_ABORT_MSG_IF(P == 0 || t0PerPlane == 0 || t1PerPlane == 0 || topo.numServers == 0,
                    "planes, servers, t0 and t1 must be at least 1");
    NS_ABORT_MSG_IF(topo.numServers % t0PerPlane != 0,
                    "numServers must be a multiple of the T0 switches per plane");
    // T0 uSIDs 0x0100.. must stay below the T1 uSIDs 0x0200..
    NS_ABORT_MSG_IF(t0PerPlane > uint32_t(T1Usid(0) - T0Usid(0)), "at most 256 T0 switches per plane");
    NS_ABORT_MSG_IF(t1PerPlane > uint32_t(0x10000 - T1Usid(0)), "too many T1 switches per plane");
    serversPerT0 = topo.numServers / t0PerPlane;

    servers.resize(topo.numServers);
    for (auto& server : servers)
    {
        server = CreateObject<Node>();
    }
    nics.assign(topo.numServers, std::vector<Ptr<PointToPointNetDevice>>(P));
    t0s.assign(P, {});
    t1s.assign(P, {});

    PointToPointHelper link;
    link.SetDeviceAttribute("DataRate", StringValue(topo.linkRate));
    link.SetChannelAttribute("Delay", StringValue(topo.linkDelay));

    for (uint32_t p = 0; p < P; p++)
    {
        uint32_t locator = PlaneLocator(p);
        for (uint32_t i = 0; i < t0PerPlane; i++)
        {
            t0s[p].push_back(CreateSwitch(p, T0Usid(i), 1));
        }
        for (uint32_t j = 0; j < t1PerPlane; j++)
        {
            t1s[p].push_back(CreateSwitch(p, T1Usid(j), 2));
            t1s[p].back()->SetIntVersion(intVersion);
        }

        ConnectServers(p, link);

        // T0 <-> T1 full mesh
        for (uint32_t i = 0; i < t0PerPlane; i++)
        {
            for (uint32_t j = 0; j < t1PerPlane; j++)
            {
                Ptr<SwitchNode> t0 = t0s[p][i];
                Ptr<SwitchNode> t1 = t1s[p][j];
                fabricLinks.push_back({p, i, j, link.Install(t0, t1)});
                uint32_t t0Port = t0->GetNDevices() - 1;
                uint32_t t1Port = t1->GetNDevices() - 1;

                t0->AddSidRoute(locator, T1Usid(j), t0Port);
                t1->AddSidRoute(locator, T0Usid(i), t1Port);

                for (uint32_t s = 0; s < topo.numServers; s++)
                {
                    Ipv6Address dst = RdmaSender::GetHostAddress(s);
                    if (s / serversPerT0 == i)
                    {
                        t1->AddRoute(dst, t1Port); // down to the server's T0
                    }
                    else
                    {
                        t0->AddRoute(dst, t0Port); // ECMP over all T1 uplinks
                    }
                }
            }
        }

        SetIntMonitors(p, t1PerPlane); // uplink j = port serversPerT0 + j, to T1 j
    }

    std::cout << "Multi-plane topology: " << P << " planes x " << topo.linkRate << ", "
              << topo.numServers << " servers, " << t0PerPlane << " T0 + " << t1PerPlane
              << " T1 per plane (" << serversPerT0 << " servers per T0, oversubscription "
              << double(serversPerT0) / t1PerPlane << ":1)" << std::endl;

    if (degradeLinks > 0)
    {
        DegradeRandomLinks(degradeLinks, DataRate(degradeRate));
    }
}

#endif /* TOPOLOGY_H */
