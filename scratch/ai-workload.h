#ifndef AI_WORKLOAD_H
#define AI_WORKLOAD_H

#include "flow-schedule.h"

#include <algorithm>
#include <iomanip>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace ns3;

/**
 * AI training workload as a dependency graph (DAG) of tasks.
 *
 * A task is a network transfer (src -> dst, bytes) or a compute delay on a
 * rank; it starts when all its dependencies are done. A transfer is done when
 * the destination has received every byte (the data is usable), which is what
 * the next step of a collective waits for. Ranks are logical: SetPlacement()
 * maps them to servers (default rank = server); a transfer uses every plane
 * of its servers.
 */
/** Pipelining of ring collectives (NCCL-like). */
struct AiRingConfig
{
    uint32_t channels = 2;        //!< Independent rings; odd channels run in reverse
    uint64_t sliceBytes = 524288; //!< Max slice; a ring step is cut into slices
    uint32_t slots = 8;           //!< Slices a rank may have in flight per channel
};

class AiWorkload
{
  public:
    using RingConfig = AiRingConfig;
    using TaskId = uint32_t;
    using Deps = std::vector<TaskId>;

    /** Server of every logical rank (empty: rank r runs on server r). */
    void SetPlacement(const std::vector<uint32_t>& serverOfRank) { m_placement = serverOfRank; }

    /**
     * Transfer of \p bytes from \p src to \p dst after \p deps: one message
     * on the long-lived QP of the two servers, behind the ones already queued
     * on it. Done once the destination has every byte.
     */
    TaskId Flow(uint32_t src, uint32_t dst, uint64_t bytes, const Deps& deps, const std::string& phase)
    {
        Task task;
        task.isFlow = true;
        task.src = src;
        task.dst = dst;
        task.bytes = bytes;
        return Add(task, deps, phase);
    }

    /** Compute of \p duration on \p rank after \p deps. */
    TaskId Compute(uint32_t rank, Time duration, const Deps& deps, const std::string& phase)
    {
        Task task;
        task.src = rank;
        task.duration = duration;
        return Add(task, deps, phase);
    }

    /** Zero-time synchronization point after \p deps (e.g. a global barrier). */
    TaskId Barrier(const Deps& deps, const std::string& phase)
    {
        return Compute(0, Time(0), deps, phase);
    }

    /**
     * Pipelined ring all-reduce of \p bytes over \p ranks (reduce-scatter then
     * all-gather, 2 (D - 1) steps). The buffer is split over the channels, each
     * step of a channel into slices of at most sliceBytes. Slice j of step k is
     * forwarded as soon as slice j of step k - 1 has been received from the ring
     * predecessor, so steps overlap; a rank keeps at most slots slices in flight
     * per channel (a slot frees when its slice has been received).
     * \param startDeps per rank (same order as \p ranks), what it waits for first
     * \return per rank, the tasks after which its all-reduce is complete
     */
    std::vector<Deps> RingAllReduce(const std::vector<uint32_t>& ranks,
                                    uint64_t bytes,
                                    const std::vector<Deps>& startDeps,
                                    const std::string& phase,
                                    const RingConfig& cfg = RingConfig())
    {
        uint32_t d = ranks.size();
        if (d < 2)
        {
            return startDeps;
        }
        std::vector<Deps> done(d);
        uint32_t channels = std::max<uint32_t>(1, cfg.channels);
        uint64_t channelBytes = std::max<uint64_t>(1, (bytes + channels - 1) / channels);
        uint64_t chunk = std::max<uint64_t>(1, (channelBytes + d - 1) / d);
        uint32_t slices = std::max<uint64_t>(1, (chunk + cfg.sliceBytes - 1) / cfg.sliceBytes);
        uint64_t slice = std::max<uint64_t>(1, (chunk + slices - 1) / slices);
        uint32_t slots = std::max<uint32_t>(1, cfg.slots);

        for (uint32_t c = 0; c < channels; c++)
        {
            // Position p of the ring sends to p + 1; odd channels walk the ranks backwards
            std::vector<uint32_t> order(d);
            for (uint32_t p = 0; p < d; p++)
            {
                order[p] = (c % 2 == 0) ? p : d - 1 - p;
            }
            std::vector<std::vector<TaskId>> prev(d, std::vector<TaskId>(slices));
            std::vector<std::vector<TaskId>> sentBy(d); // per position, in send order
            for (uint32_t step = 0; step < 2 * (d - 1); step++)
            {
                std::vector<std::vector<TaskId>> cur(d, std::vector<TaskId>(slices));
                for (uint32_t j = 0; j < slices; j++)
                {
                    for (uint32_t p = 0; p < d; p++)
                    {
                        // Slice j arrived from the predecessor at the previous step
                        Deps deps = step == 0 ? startDeps[order[p]] : Deps{prev[(p + d - 1) % d][j]};
                        if (sentBy[p].size() >= slots)
                        {
                            deps.push_back(sentBy[p][sentBy[p].size() - slots]); // slot reuse
                        }
                        cur[p][j] = Flow(ranks[order[p]], ranks[order[(p + 1) % d]], slice, deps, phase);
                        sentBy[p].push_back(cur[p][j]);
                    }
                }
                prev = cur;
            }
            for (uint32_t p = 0; p < d; p++)
            {
                // Done once its last sends are delivered and its last receives are in
                Deps& rankDone = done[order[p]];
                rankDone.insert(rankDone.end(), prev[p].begin(), prev[p].end());
                rankDone.insert(rankDone.end(), prev[(p + d - 1) % d].begin(), prev[(p + d - 1) % d].end());
            }
        }
        return done;
    }

    /**
     * All-to-all of \p bytesPerRank per rank (bytesPerRank / (D - 1) to every
     * other rank), then a barrier.
     */
    TaskId AllToAll(const std::vector<uint32_t>& ranks,
                    uint64_t bytesPerRank,
                    const std::vector<Deps>& startDeps,
                    const std::string& phase)
    {
        uint32_t d = ranks.size();
        uint64_t size = std::max<uint64_t>(1, bytesPerRank / std::max<uint32_t>(1, d - 1));
        Deps flows;
        for (uint32_t s = 0; s < d; s++)
        {
            for (uint32_t r = 0; r < d; r++)
            {
                if (r != s)
                {
                    flows.push_back(Flow(ranks[s], ranks[r], size, startDeps[s], phase));
                }
            }
        }
        return Barrier(flows, phase);
    }

    /**
     * Permutation: every rank sends \p bytes to a distinct random peer (a
     * derangement, fixed by \p seed), then a barrier.
     */
    TaskId Permutation(const std::vector<uint32_t>& ranks,
                       uint64_t bytes,
                       const std::vector<Deps>& startDeps,
                       const std::string& phase,
                       uint32_t seed)
    {
        uint32_t d = ranks.size();
        std::vector<uint32_t> peer(d);
        std::mt19937 rng(seed);
        bool fixedPoint = true;
        while (fixedPoint)
        {
            for (uint32_t i = 0; i < d; i++)
            {
                peer[i] = i;
            }
            std::shuffle(peer.begin(), peer.end(), rng);
            fixedPoint = false;
            for (uint32_t i = 0; i < d; i++)
            {
                fixedPoint |= peer[i] == i;
            }
        }
        Deps flows;
        for (uint32_t s = 0; s < d; s++)
        {
            flows.push_back(Flow(ranks[s], ranks[peer[s]], bytes, startDeps[s], phase));
        }
        return Barrier(flows, phase);
    }

    /** Launch every task without dependencies. */
    void Start()
    {
        for (TaskId id = 0; id < m_tasks.size(); id++)
        {
            if (m_tasks[id].waiting == 0)
            {
                Launch(id);
            }
        }
    }

    uint32_t GetNTasks() const { return m_tasks.size(); }
    uint32_t GetNDone() const { return m_done; }

    /** Count phase \p phase as one collective in ReportCollectives(). */
    void MarkCollective(const std::string& phase) { m_collectives.push_back(phase); }

    /**
     * Completion time (last task end - first task start) of every collective
     * marked with MarkCollective(): count, mean, median, p99, min, max (us).
     */
    void ReportCollectives(std::ostream& os) const
    {
        std::vector<double> times;
        for (const auto& name : m_collectives)
        {
            const Phase& p = m_phases.at(name);
            if (p.tasks > 0 && p.done == p.tasks)
            {
                times.push_back((p.end - p.start) / 1e3);
            }
        }
        os << "Collectives: " << times.size() << "/" << m_collectives.size() << " done";
        if (!times.empty())
        {
            std::sort(times.begin(), times.end());
            double sum = 0;
            for (double t : times)
            {
                sum += t;
            }
            size_t n = times.size();
            double median = n % 2 ? times[n / 2] : (times[n / 2 - 1] + times[n / 2]) / 2;
            os << std::fixed << std::setprecision(3) << ", completion (us): mean " << sum / n
               << " median " << median << " p99 " << times[std::min(n - 1, size_t(0.99 * n))]
               << " min " << times.front() << " max " << times.back();
        }
        os << std::endl;
    }

    /** Per phase (in first-start order): first start, last end, task count. */
    void Report(std::ostream& os) const
    {
        os << "phase,start_us,end_us,duration_us,tasks,done" << std::endl;
        os << std::fixed << std::setprecision(3);
        for (const auto& name : m_phaseOrder)
        {
            const Phase& p = m_phases.at(name);
            os << name << "," << p.start / 1e3 << "," << p.end / 1e3 << ","
               << (p.end - p.start) / 1e3 << "," << p.tasks << "," << p.done << std::endl;
        }
    }

  private:
    struct Task
    {
        bool isFlow{false};
        uint32_t src{0}; //!< Source rank, or the computing rank
        uint32_t dst{0};
        uint64_t bytes{0};
        Time duration;
        std::string phase;
        uint32_t waiting{0};
        std::vector<TaskId> next;
    };

    struct Phase
    {
        int64_t start{-1}; //!< ns
        int64_t end{-1};
        uint32_t tasks{0};
        uint32_t done{0};
    };

    TaskId Add(Task task, const Deps& deps, const std::string& phase)
    {
        TaskId id = m_tasks.size();
        task.phase = phase;
        task.waiting = deps.size();
        m_tasks.push_back(task);
        for (TaskId dep : deps)
        {
            NS_ABORT_MSG_IF(dep >= id, "Task dependencies must be created first");
            m_tasks[dep].next.push_back(id);
        }
        if (m_phases.find(phase) == m_phases.end())
        {
            m_phaseOrder.push_back(phase);
        }
        m_phases[phase].tasks++;
        return id;
    }

    void Launch(TaskId id)
    {
        Task& task = m_tasks[id];
        Phase& phase = m_phases[task.phase];
        int64_t now = Simulator::Now().GetNanoSeconds();
        if (phase.start < 0 || now < phase.start)
        {
            phase.start = now;
        }
        if (task.isFlow)
        {
            NS_ABORT_MSG_IF(task.bytes > UINT32_MAX, "Transfer larger than 4 GB");
            uint32_t msgId = SendMessage(ServerOf(task.src),
                                         ServerOf(task.dst),
                                         task.bytes,
                                         MakeCallback(&AiWorkload::OnReceived, this));
            m_messageTask[msgId] = id;
        }
        else
        {
            Simulator::Schedule(task.duration, &AiWorkload::Done, this, id);
        }
    }

    void OnReceived(uint32_t msgId)
    {
        auto it = m_messageTask.find(msgId);
        NS_ABORT_MSG_IF(it == m_messageTask.end(), "Unknown message " << msgId);
        TaskId id = it->second;
        m_messageTask.erase(it);
        Done(id);
    }

    void Done(TaskId id)
    {
        Task& task = m_tasks[id];
        Phase& phase = m_phases[task.phase];
        phase.end = std::max(phase.end, Simulator::Now().GetNanoSeconds());
        phase.done++;
        m_done++;
        for (TaskId next : task.next)
        {
            if (--m_tasks[next].waiting == 0)
            {
                Launch(next);
            }
        }
    }

    uint32_t ServerOf(uint32_t rank) const
    {
        return m_placement.empty() ? rank : m_placement.at(rank);
    }

    std::vector<uint32_t> m_placement; //!< Logical rank -> server
    std::vector<Task> m_tasks;
    std::map<std::string, Phase> m_phases;
    std::vector<std::string> m_phaseOrder;
    std::vector<std::string> m_collectives; //!< Phases reported as collectives
    std::map<uint32_t, TaskId> m_messageTask; //!< In-flight message id -> task
    uint32_t m_done{0};
};

/** Parameters of the built-in workloads (pfc.cc --workload). */
struct AiWorkloadConfig
{
    std::string type = "training"; //!< training | allreduce | alltoall | permutation | trace
    std::string placement = "strided"; //!< packed | strided | random (rank -> server)
    AiWorkload::RingConfig ring;
    uint64_t bytes = 256000000;    //!< allreduce / alltoall / permutation: bytes per rank
    uint32_t groupSize = 16;       //!< allreduce / alltoall / permutation: ranks per collective
                                   //!< (consecutive ranks, all groups at once), 0: all ranks
    uint32_t iterations = 2;
    // training: pipeline parallel (pp stages) x data parallel (servers / pp)
    uint32_t pp = 4;               //!< Pipeline stages
    uint32_t microbatches = 4;
    uint64_t activationBytes = 8000000; //!< Per micro-batch between stages (fwd and bwd)
    uint64_t gradientBytes = 64000000;  //!< Per rank, all-reduced within each stage group
    double forwardUs = 500;             //!< Compute per micro-batch
    double backwardUs = 1000;
};

/**
 * Pipeline + data-parallel training, GPipe schedule, per iteration:
 *   forward of micro-batches 0..M-1 through stages 0..P-1 (activations to the
 *   next stage), backward M-1..0 through P-1..0 (gradients to the previous
 *   stage), ring all-reduce of the gradients among the D replicas of each
 *   stage, then a global barrier (optimizer step) before the next iteration.
 * Rank of stage s in replica d: s * D + d.
 */
void
BuildTraining(AiWorkload& w, const AiWorkloadConfig& cfg, uint32_t numRanks)
{
    const uint32_t P = cfg.pp;
    NS_ABORT_MSG_IF(P == 0 || numRanks % P != 0, "servers must be a multiple of pp");
    const uint32_t D = numRanks / P;
    const uint32_t M = cfg.microbatches;
    // A stage's data-parallel group is D consecutive ranks: with strided
    // placement it spreads over the T0s, and pipeline neighbours (D apart,
    // a multiple of the T0 count) share a T0
    auto rank = [D](uint32_t s, uint32_t d) { return s * D + d; };

    AiWorkload::Deps barrier; // previous iteration's global barrier
    for (uint32_t it = 0; it < cfg.iterations; it++)
    {
        std::string tag = "iter" + std::to_string(it);
        std::vector<AiWorkload::Deps> stageDone(P * D); // per rank, last backward
        for (uint32_t d = 0; d < D; d++)
        {
            // fwd[s][m], act[s][m]: activation of micro-batch m from stage s to s+1
            std::vector<std::vector<AiWorkload::TaskId>> fwd(P, std::vector<AiWorkload::TaskId>(M));
            std::vector<std::vector<AiWorkload::TaskId>> act(P, std::vector<AiWorkload::TaskId>(M));
            for (uint32_t m = 0; m < M; m++)
            {
                for (uint32_t s = 0; s < P; s++)
                {
                    AiWorkload::Deps deps = barrier;
                    if (m > 0)
                    {
                        deps.push_back(fwd[s][m - 1]); // one compute at a time per rank
                    }
                    if (s > 0)
                    {
                        deps.push_back(act[s - 1][m]);
                    }
                    fwd[s][m] = w.Compute(rank(s, d), MicroSeconds(cfg.forwardUs), deps, tag + ".forward");
                    if (s + 1 < P)
                    {
                        act[s][m] = w.Flow(rank(s, d), rank(s + 1, d), cfg.activationBytes,
                                           {fwd[s][m]}, tag + ".pipeline");
                    }
                }
            }
            // Backward in reverse micro-batch order; grad[s][m] goes from stage s to s-1
            std::vector<std::vector<AiWorkload::TaskId>> bwd(P, std::vector<AiWorkload::TaskId>(M));
            std::vector<std::vector<AiWorkload::TaskId>> grad(P, std::vector<AiWorkload::TaskId>(M));
            for (int32_t m = M - 1; m >= 0; m--)
            {
                for (int32_t s = P - 1; s >= 0; s--)
                {
                    AiWorkload::Deps deps;
                    deps.push_back(m == int32_t(M) - 1 ? fwd[s][M - 1] : bwd[s][m + 1]);
                    if (s + 1 < int32_t(P))
                    {
                        deps.push_back(grad[s + 1][m]);
                    }
                    bwd[s][m] = w.Compute(rank(s, d), MicroSeconds(cfg.backwardUs), deps, tag + ".backward");
                    if (s > 0)
                    {
                        grad[s][m] = w.Flow(rank(s, d), rank(s - 1, d), cfg.activationBytes,
                                            {bwd[s][m]}, tag + ".pipeline");
                    }
                }
            }
            for (uint32_t s = 0; s < P; s++)
            {
                stageDone[rank(s, d)] = {bwd[s][0]};
            }
        }

        // Data-parallel gradient all-reduce inside each stage group, then a global barrier
        AiWorkload::Deps finals;
        for (uint32_t s = 0; s < P; s++)
        {
            std::vector<uint32_t> group;
            std::vector<AiWorkload::Deps> starts;
            for (uint32_t d = 0; d < D; d++)
            {
                group.push_back(rank(s, d));
                starts.push_back(stageDone[rank(s, d)]);
            }
            std::string phase = tag + ".allreduce.s" + std::to_string(s);
            w.MarkCollective(phase);
            for (auto& done : w.RingAllReduce(group, cfg.gradientBytes, starts, phase, cfg.ring))
            {
                finals.insert(finals.end(), done.begin(), done.end());
            }
        }
        barrier = {w.Barrier(finals, tag + ".end")};
    }
}

/**
 * Server of every rank:
 *   packed:  rank r on server r (neighbouring ranks share a T0)
 *   strided: consecutive ranks on consecutive T0s (rank r on T0 r % T0s): a
 *            group of T0s consecutive ranks (e.g. a data-parallel ring) has one
 *            GPU on every T0, and ranks T0s apart share a T0
 *   random:  a fixed random permutation
 */
std::vector<uint32_t>
MakePlacement(const std::string& type, uint32_t numRanks)
{
    std::vector<uint32_t> server(numRanks);
    for (uint32_t r = 0; r < numRanks; r++)
    {
        server[r] = r;
    }
    if (type == "packed")
    {
        return server;
    }
    if (type == "strided")
    {
        uint32_t t0s = topo.numT0PerPlane;
        NS_ABORT_MSG_IF(numRanks != topo.numServers, "strided placement needs one rank per server");
        for (uint32_t r = 0; r < numRanks; r++)
        {
            server[r] = (r % t0s) * serversPerT0 + r / t0s;
        }
        return server;
    }
    if (type == "random")
    {
        std::mt19937 rng(1);
        std::shuffle(server.begin(), server.end(), rng);
        return server;
    }
    NS_ABORT_MSG("Unknown placement " << type);
    return server;
}

/** Build the configured workload on ranks 0..numRanks-1. */
void
BuildWorkload(AiWorkload& w, const AiWorkloadConfig& cfg, uint32_t numRanks)
{
    w.SetPlacement(MakePlacement(cfg.placement, numRanks));
    std::vector<uint32_t> ranks(numRanks);
    for (uint32_t r = 0; r < numRanks; r++)
    {
        ranks[r] = r;
    }
    if (cfg.type == "training")
    {
        BuildTraining(w, cfg, numRanks);
        return;
    }
    // Groups of groupSize consecutive ranks each run their own collective, all
    // at once; a global barrier starts every group's next iteration together
    uint32_t size = cfg.groupSize ? cfg.groupSize : numRanks;
    NS_ABORT_MSG_IF(numRanks % size != 0, "ranks must be a multiple of groupSize");
    uint32_t groups = numRanks / size;
    AiWorkload::Deps barrier;
    for (uint32_t it = 0; it < cfg.iterations; it++)
    {
        std::string tag = "iter" + std::to_string(it) + "." + cfg.type;
        AiWorkload::Deps finals;
        for (uint32_t g = 0; g < groups; g++)
        {
            std::vector<uint32_t> group(ranks.begin() + g * size, ranks.begin() + (g + 1) * size);
            std::vector<AiWorkload::Deps> starts(size, barrier);
            std::string phase = tag + ".g" + std::to_string(g);
            w.MarkCollective(phase);
            if (cfg.type == "allreduce")
            {
                for (auto& done : w.RingAllReduce(group, cfg.bytes, starts, phase, cfg.ring))
                {
                    finals.insert(finals.end(), done.begin(), done.end());
                }
            }
            else if (cfg.type == "alltoall")
            {
                finals.push_back(w.AllToAll(group, cfg.bytes, starts, phase));
            }
            else if (cfg.type == "permutation")
            {
                finals.push_back(w.Permutation(group, cfg.bytes, starts, phase, it * groups + g + 1));
            }
            else
            {
                NS_ABORT_MSG("Unknown workload " << cfg.type);
            }
        }
        barrier = {w.Barrier(finals, tag + ".end")};
    }
}

#endif /* AI_WORKLOAD_H */
