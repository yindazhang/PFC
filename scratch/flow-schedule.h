#ifndef FLOW_SCHEDULER_H
#define FLOW_SCHEDULER_H

#include "topology.h"

#include <cstdio>
#include <unordered_map>
#include <vector>

using namespace ns3;

FILE* logFilePtr = nullptr;
FILE* flowFilePtr = nullptr;

uint32_t numQps = 0;
uint32_t numMessages = 0;

/** The long-lived QP from one server to another. */
struct Connection
{
    Ptr<RdmaSender> sender;
    Ptr<RdmaReceiver> receiver;
};

std::vector<std::vector<Connection>> connections; //!< [src][dst] -> QP (none for src == dst)

/** A message in flight, until its sender has every byte acknowledged. */
struct MessageRecord
{
    uint32_t qp;
    uint32_t src;
    uint32_t dst;
    uint32_t size;
    int64_t postTime;        //!< ns
    int64_t receiveTime{-1}; //!< ns: the destination has every byte
    Callback<void, uint32_t> onReceived;
};

std::unordered_map<uint32_t, MessageRecord> messages; //!< Message id -> record

void
OnMessageReceived(uint32_t msgId)
{
    MessageRecord& msg = messages.at(msgId);
    msg.receiveTime = Simulator::Now().GetNanoSeconds();
    if (!msg.onReceived.IsNull())
    {
        msg.onReceived(msgId);
    }
}

/** FCT log line: id,qp,src,dst,size,post_ns,receive_ns,ack_ns,fct_ns (fct: post to receive). */
void
OnMessageAcked(uint32_t msgId)
{
    auto it = messages.find(msgId);
    NS_ABORT_MSG_IF(it == messages.end() || it->second.receiveTime < 0,
                    "Message " << msgId << " acknowledged before it was received");
    const MessageRecord& msg = it->second;
    if (logFilePtr != nullptr)
    {
        fprintf(logFilePtr,
                "%u,%u,%u,%u,%u,%ld,%ld,%ld,%ld\n",
                msgId,
                msg.qp,
                msg.src,
                msg.dst,
                msg.size,
                msg.postTime,
                msg.receiveTime,
                Simulator::Now().GetNanoSeconds(),
                msg.receiveTime - msg.postTime);
    }
    messages.erase(it);
}

/**
 * Connect every ordered pair of servers with one QP (numbered in (src, dst)
 * order): its sender on every plane NIC of src, its receiver on every plane
 * NIC of dst. Call once, after the topology is built; every message between
 * two servers then reuses their QP.
 */
void
BuildConnections()
{
    uint32_t n = servers.size();
    connections.assign(n, std::vector<Connection>(n));
    for (uint32_t src = 0; src < n; src++)
    {
        for (uint32_t dst = 0; dst < n; dst++)
        {
            if (src == dst)
            {
                continue;
            }
            QpInfo qp;
            qp.id = ++numQps;
            qp.src = src;
            qp.dst = dst;
            qp.minRttNs = BaseRttNs(src, dst);

            Connection& conn = connections[src][dst];
            conn.receiver = CreateObject<RdmaReceiver>(qp.id);
            conn.receiver->SetMessageCallback(MakeCallback(&OnMessageReceived));
            for (auto& nic : nics[dst])
            {
                nic->AddReceiver(conn.receiver);
            }

            conn.sender = CreateObject<RdmaSender>(qp);
            conn.sender->SetMessageCallback(MakeCallback(&OnMessageAcked));
            if (sourceRouting)
            {
                // Paths first: each plane builds its EV set when its NIC registers
                for (auto& [plane, path] : SourceRoutes(src, dst))
                {
                    conn.sender->AddPath(plane, path);
                }
            }
            for (auto& nic : nics[src])
            {
                nic->AddSender(conn.sender);
            }
        }
    }
}

/**
 * Send a message of \p size bytes from \p src to \p dst now, on their QP
 * (after the messages already queued on it). \p onReceived (optional) is
 * invoked with the message id once the destination has every byte.
 * \return the message id.
 */
uint32_t
SendMessage(uint32_t src,
            uint32_t dst,
            uint32_t size,
            Callback<void, uint32_t> onReceived = Callback<void, uint32_t>())
{
    NS_ABORT_MSG_IF(src >= connections.size() || dst >= connections.size() || src == dst,
                    "Invalid message " << src << " -> " << dst << " ("
                                       << connections.size() << " connected servers)");
    Connection& conn = connections[src][dst];
    uint32_t msgId = ++numMessages;
    messages.emplace(msgId,
                     MessageRecord{conn.sender->GetId(),
                                   src,
                                   dst,
                                   size,
                                   Simulator::Now().GetNanoSeconds(),
                                   -1,
                                   onReceived});
    conn.sender->PostMessage(msgId, size);
    return msgId;
}

bool
OpenFctLog()
{
    logFilePtr = fopen((logFile + ".fct").c_str(), "w");
    if (logFilePtr == nullptr)
    {
        std::cerr << "Cannot open " << logFile << ".fct" << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Trace replay: lines "src dst size start_ns", no dependencies between messages
// ---------------------------------------------------------------------------

void
ReadLine()
{
    uint32_t src;
    uint32_t dst;
    uint32_t size;
    uint64_t startTime;
    if (fscanf(flowFilePtr, "%u %u %u %lu", &src, &dst, &size, &startTime) != 4)
    {
        return;
    }
    // Always schedule (never recurse) so flows with the same start time are fine
    Time start = NanoSeconds(startTime);
    Simulator::Schedule(start > Simulator::Now() ? start - Simulator::Now() : Time(0),
                        [src, dst, size]() {
                            SendMessage(src, dst, size);
                            ReadLine();
                        });
}

bool
ScheduleFlow()
{
    std::string traceName = "trace/" + flowFile + ".tr";
    flowFilePtr = fopen(traceName.c_str(), "r");
    if (flowFilePtr == nullptr)
    {
        std::cerr << "Cannot open flow file " << traceName << std::endl;
        return false;
    }
    ReadLine();
    return true;
}

#endif /* FLOW_SCHEDULER_H */
