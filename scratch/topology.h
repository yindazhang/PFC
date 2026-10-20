#ifndef TOPOLOGY_H
#define TOPOLOGY_H

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <random>
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

/**
 * Multi-plane topology: every server has one NIC per plane and each plane is
 * an independent two-tier Clos (T0 leaf, T1 spine).
 */
struct MultiPlaneConfig
{
    uint32_t numPlanes = 4;
    uint32_t numServers = 256;
    uint32_t numT0PerPlane = 16;
    uint32_t numT1PerPlane = 16; //!< Non-blocking when equal to the servers per T0
    std::string linkRate = "100Gbps"; //!< Per plane, all links
    std::string linkDelay = "1us";
};

MultiPlaneConfig topo;

std::vector<Ptr<Node>> servers;
std::vector<std::vector<Ptr<PointToPointNetDevice>>> nics; //!< [server][plane]
std::vector<std::vector<Ptr<SwitchNode>>> t0s;              //!< [plane][t0]
std::vector<std::vector<Ptr<SwitchNode>>> t1s;              //!< [plane][t1]

uint32_t serversPerT0 = 0;

struct FabricLink
{
    uint32_t plane;
    uint32_t t0;
    uint32_t t1;
    NetDeviceContainer devices;
};

std::vector<FabricLink> fabricLinks; //!< All T0-T1 links

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

/** Same-T0 flows cross 2 links each way, others 4. */
uint64_t
BaseRttNs(uint32_t src, uint32_t dst)
{
    Time delay(topo.linkDelay);
    uint32_t hops = (src / serversPerT0 == dst / serversPerT0) ? 2 : 4;
    return 2 * hops * delay.GetNanoSeconds();
}

/**
 * SRv6 paths (one per EV) from src to dst on every plane: [T0] within a T0,
 * otherwise [T0 src, T1 j, T0 dst] for every T1 j of the plane.
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
    NS_ABORT_MSG_IF(count > fabricLinks.size(), "degradeLinks exceeds the number of T0-T1 links");
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
        std::cout << "Degraded link to " << rate << ": plane " << link.plane << " T0 " << link.t0
                  << " - T1 " << link.t1 << std::endl;
    }
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
    std::vector<uint64_t> allUp;
    std::vector<uint64_t> allDown;
    std::vector<uint64_t> degradedUp; //!< Over degraded links only (below the plane rate)
    std::vector<uint64_t> healthyUp;
    DataRate planeRate(topo.linkRate);
    for (uint32_t p = 0; p < topo.numPlanes; p++)
    {
        std::vector<uint64_t> up;
        std::vector<uint64_t> down;
        for (auto& t0 : t0s[p])
        {
            for (uint32_t j = 0; j < topo.numT1PerPlane; j++)
            {
                uint64_t bytes = t0->GetTxBytes(serversPerT0 + j);
                up.push_back(bytes);
                auto dev = DynamicCast<PointToPointNetDevice>(t0->GetDevice(serversPerT0 + j));
                (dev->GetDataRate() < planeRate ? degradedUp : healthyUp).push_back(bytes);
            }
        }
        for (auto& t1 : t1s[p])
        {
            for (uint32_t i = 0; i < topo.numT0PerPlane; i++)
            {
                down.push_back(t1->GetTxBytes(i));
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

void
BuildMultiPlane()
{
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
        }

        // Servers <-> T0
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
