# MRC multi-plane simulator: project notes

Handoff notes for anyone, including Claude in a new session, picking up this work.
They cover what exists, why it was built that way, what has been verified, and
what is still open. How to run and analyse experiments is in [README.md](README.md).

Last updated: 2026-09-24.

## 0. Repository layout and workflow

- **This repo (`github.com/yindazhang/PFC`)** holds only the changed parts of
  ns-3.46: `src/` (in practice only `src/point-to-point/` differs from stock),
  `scratch/`, and `commands/`.
- **`commands/install.sh`** clones or pulls this repo into `/mydata/PFC`,
  unpacks ns-3.46 into `/mydata/ns-3.46`, copies `src/` and `scratch/` over it,
  then configures an optimized build (`build/optimized`) and builds.
- **Day-to-day editing happens in `/mydata/ns-3.46`.** Before committing, sync the
  changed trees back:
  ```bash
  rsync -a --delete /mydata/ns-3.46/src/point-to-point/ /mydata/PFC/src/point-to-point/
  rsync -a --delete /mydata/ns-3.46/scratch/ /mydata/PFC/scratch/
  ```
- **`commands/` scripts** find the ns-3 tree on their own: `$NS3_DIR`, else
  `../ns-3.46` next to the repo.
- **Results** go to `/mydata/ns-3.46/results/<experiment>/`. They are not in git.
- **Binary:** `build/optimized/scratch/ns3.46-pfc-optimized`, run with
  `LD_LIBRARY_PATH=build/optimized/lib`. `run.sh` does this for you.

## 1. What is modelled

An OpenAI/OCP **MRC** (Multipath Reliable Connection) style RoCEv2 fabric over
IPv6, with **multiple planes**, driven by an **AI workload engine**.

### Topology (`scratch/topology.h`), defaults

- 4 planes × 100 Gbps. Each server (= one GPU) has one NIC port per plane, so 400 Gbps per GPU.
- 256 servers.
- Per plane, 16 T0 and 16 T1 switches: 16 servers per T0, 1:1 oversubscription.
- Every link is 1 µs.
- All of it is set at run time: `--planes`, `--servers`, `--t0`, `--t1`
  (servers a multiple of t0, t0 ≤ 256 because of the uSID ranges). `--t1` below
  servers/t0 oversubscribes, above it undersubscribes. The `srv6all_trim` preset
  uses `EvsPerPlane=65535`, clamped to the paths per plane, so "all" follows `--t1`.
  Log names include the topology (`_P4_S256_T16x16_`).
- Addresses: host `fd00::<id>` (`RdmaSender::GetHostAddress`). SRv6 plane locator
  `0xfc00000p`; T0 uSID `0x0100+i`, T1 uSID `0x0200+j`.
- `--degradeLinks=N`: N random T0–T1 links run at `--degradeRate` (50 Gbps),
  chosen with `mt19937(1)`. This is how failures are modelled (asymmetric bandwidth).

### Headers (`src/point-to-point/model/`)

- **`bth-header`**: IBTA BTH (24-bit PSN, `BthHeader::UnwrapPsn` for long-lived
  QPs), AETH, and the MRC extensions:
  - request stack: METH, TSETH, RETH, ImmDt;
  - SACK: SETH + CC_STATE, with `FLAG_DUPLICATE = 0x04`;
  - NACK: NETH (trimmed / out of MPR);
  - probes and endpoint ops: PETH, ERTH, EETH;
  - opcodes 0x50–0x5C.

  The MRC field layouts follow the spec loosely and have **not** been checked
  byte for byte.
- **`srv6-header`**: compressed SID (`Srv6Sid` = uint32 locator + uint16
  usid[6], NEXT-CSID style), and an SRH with `Match` / `Shift`. Simplification:
  the SRH carries the full path and is shifted in place.
- **`pfc-header`**: priority + pause/resume (opcode 0x0101).
- **`packet-tag`**: size + ingress device.

### Switch (`switch-node`)

- **IPv6 only**; IPv4 aborts. ECMP hashes IPv6 fields only (src, dst, flow label; `Hash32`).
- **Two priorities** (`point-to-point-queue`), classified by DSCP:
  - CS6 = high: ACK / SACK / NACK, trimmed packets, PFC. Assumed never to drop.
  - Everything else = low (data), which is subject to the MMU, ECN and PFC.
- **MMU:**
  - dynamic-threshold shared buffer, α = 1;
  - 5 KB per Gbps of port buffer (500 KB per 100G port);
  - 10 KB reserved per port;
  - per-port headroom;
  - PFC pause/resume on the low class;
  - admission: with PFC, per ingress port (reserved, shared, headroom). Without
    PFC (the default), per egress queue (reserved, shared; no headroom), as
    shared-buffer switches do for lossy traffic (`--egressAdmission=0` restores
    ingress; changed 2026-09-30). With α = 1 a single hot queue can still take
    about half the pool (about 7 MB on a T0), so trims stay rare and queues can
    outlast the then 200 µs MinRto (1.57 MB peak and 141 RTOs in a 16 MB all-to-all
    with 256 degraded links). The largest queue is printed (`max queue`).
- **ECN:** RED with Kmin 0.2 / Kmax 0.8 of the port buffer (100 / 400 KB at
  100G) and Pmax 1.0, as FASTFLOW / UEC htsim, marked at dequeue. Settable:
  `--ecnKmin` / `--ecnKmax` (KB for a 100 Gbps port) and `--ecnPmax`. (Until
  2026-10-01 the defaults were 0.1 / 0.4 and Pmax 0.2, i.e. 50 / 200 KB; in a
  32 MB spray all-reduce, 50 KB marked 8× more than 100 KB for no gain, 25 KB
  was 2% slower. Results before that date used the old values.) The 2-bit IPv6 ECN field holds two independent mark bits instead
  of the RFC 3168 codepoints (`PointToPointQueue::ECN_MARK_*`; senders send 00):
  - 0x02 = ECN, read only by congestion control (NSCC); `--ecnMark`, default
    on, **off with `--srcRoute`**, so SRv6 congestion control runs on RTT only;
  - 0x01 = load-balancing (LB) mark, read only by SRv6 senders for path
    switching; `--lbMark`, default on (spray senders ignore it).
  The two bits are decided separately: ECN by RED (below), the LB bit by a
  step at `--lbThreshold` (KB for a 100 Gbps port, default 100): every
  packet while the egress queue is above it, none below, no randomness.
  (Until 2026-10-01 one RED decision set both.) `--lbMode=flow` instead marks
  only the connections with hash(src, dst, switch, port) / 2^32 <
  (queue − threshold) / port buffer, so a share of connections that grows
  with the queue moves away, rather than all of them; the hash has its own
  seed and is salted per switch and port so the same connections are not
  always the ones pushed. `--lbMode=packet` uses the same p but draws it per
  packet (own random stream per switch, so ECN's draws are unchanged), to
  separate the effect of per-connection vs per-packet marking from the
  probability curve. `--ecnIgnoreOnLb=1` makes congestion control ignore the
  ECN of a packet that also carried the LB mark (treated as unmarked, so NSCC
  may increase), leaving path congestion to path switching.
  SRv6 path-switching triggers can be turned off one by one: `--evOnLb` (the
  LB mark) and `--evOnLoss` (trim NACK and RTO), both on by default.
  Reorder-detected losses never switch paths. The ECN bit is on every port or none (the
  `--lastHopEcn` option was removed 2026-10-01); the LB bit is set on every
  fabric port (T0 → T1 and T1 → T0) but never on the last hop (T0 → server),
  which all paths share.
  (Changed 2026-10-01. Spray runs are bit-identical to before the split, and
  so are SRv6 runs at the default, ECN off. SRv6 with `--ecnMark=1` now also
  marks ECN on the last hop.)
- **Trimming** (`--trim`): on buffer overflow, the packet becomes a fresh
  header-only packet sent at high priority.
- **SRv6:** switch identity = (plane locator, switch uSID). A switch forwards
  on the active SID; otherwise it uses its IPv6 route table.
- Buffer sizes are compile-time constants (`switch-node.h`); the ECN thresholds are options (`--ecnKmin` / `--ecnKmax` / `--ecnPmax`).

### NIC and transport

- **`point-to-point-net-device`:**
  - `AddSender` / `AddReceiver` register a QP.
  - The NIC pulls packets from senders in order of their next send time (a
    priority queue) when the link is idle and the low class is not paused.
  - It demultiplexes RDMA packets by DestQP.
- **`rdma-sender`** (requester), one per **long-lived QP**:
  - **Messages:** `PostMessage(msgId, size)` queues a message on one continuous
    PSN stream (RC order). The last packet is `WRITE_LAST/ONLY_WITH_IMM`, with
    ImmDt = message id.
  - **Multi-plane:** each NIC calls `AddPlane()`. The window and MPR are shared,
    and pacing is at the sum of the plane rates.
  - **Reliability:**
    - selective: SACK bitmap plus the triggering PSN;
    - RACK-like loss detection: a packet is lost if a later-sent one was acked
      and it has been out longer than srtt + ReorderWindow × baseRTT, with the
      window doubling (up to 16) on a duplicate SACK or a late ack. The window
      starts at 8 base RTTs (was 1 until 2026-09-30: about 20k false positives
      in a 2 MB all-to-all with 256 degraded links; 4 left about 200, 8 left 0);
    - RTO from 400 µs, backing off up to 2 ms (was 200 µs until 2026-09-30: queues of
      1.6–3.2 MB on 50G degraded links outlast it, so RTOs fired without any loss);
    - trim NACK → retransmit.
  - **Congestion control: NSCC (UEC / SMaRTT):**
    - four ECN × RTT cases;
    - multiplicative decrease at most once per base RTT;
    - fair increase, proportional increase, Fast Increase;
    - Quick Adapt on trim or RTO;
    - base RTT = the minimum sample, target = 1.5 × base;
    - window in [MTU, 1.5 BDP];
    - initial window = BDP at the full multi-plane rate.
  - **Path selection:**
    - per-flow ECMP (`--spray=0`), or per-packet spraying with hashed flow labels (`--spray=1`);
    - or **SRv6 source routing + EV management** (`--srcRoute=1`):
      - every EV is one SRv6 path;
      - per plane, half the paths are active (the same count on every plane,
        chosen at random within the plane) and the rest form the plane's
        backup queue (FIFO);
      - every active EV is sprayed in turn (no skipping); the target is half
        of the plane's paths (rounded up), also for `srv6all_trim`;
      - an LB mark (not ECN), a trim NACK, or an RTO (on the oldest outstanding packet) for an
        active EV → it leaves the active set and goes to the back of the
        backup queue;
      - whenever a plane is below its target, the queue front rejoins the
        active set. With half active, it replaces a signalled EV at once; with
        all active, signalled paths are left out until half remain, then they
        rotate like the half case;
      - no skip / ASSUMED_BAD states: without EV probing a bad path would
        never return, so every EV comes back eventually. Signals for an EV
        that is already a backup are ignored. (Changed 2026-09-30; replaced
        `--evSkip`, EcnSkipRtts, ASSUMED_BAD and EvMinActive; a hold time
        `--evHold` was tried and removed the same day.) Counted as
        `EV swaps (N rejoined)`;
      - reorder-detected losses and duplicate SACKs don't touch EVs: under
        spraying they are reordering, not failed paths (changed 2026-09-30:
        they used to retire / restore EVs, about 20k spurious retire/restore
        pairs in a 2 MB all-to-all with 256 degraded links);
      - no EV probing: a retired EV never comes back.
  - Global counters: `RdmaSender::GetGlobalStats()`.
- **`rdma-receiver`** (responder):
  - accepts packets out of order within the MPR (1024);
  - answers every data packet with one SACK or NACK (no coalescing), echoing the flow label;
  - completes messages in PSN order via `SetMessageCallback(msgId)`.

### Scratch: application layer (`scratch/`)

- **`flow-schedule.h`:**
  - `BuildConnections()` creates **one QP per ordered (src, dst) pair at startup**
    (256 × 255 = 65,280 QPs, about 12 KB each, about 0.8–1 GB in total). It is
    called in `pfc.cc` right after `BuildMultiPlane()`.
  - `SendMessage(src, dst, size, onReceived)` posts onto the pair's QP.
  - Per-message FCT log `logs/*.fct`: `id,qp,src,dst,size,post_ns,receive_ns,ack_ns,fct_ns`,
    where fct = post → receive.
- **`ai-workload.h`**, a dependency-graph engine:
  - Tasks: `Flow` (one message, done when the receiver has every byte),
    `Compute`, `Barrier`. A task launches when all its dependencies are done.
  - Collectives:
    - `RingAllReduce`: pipelined; `channels` rings (odd ones reversed), each
      step cut into ≤ `sliceBytes` slices, `slots` slices in flight per rank and channel;
    - `AllToAll`;
    - `Permutation`: a derangement, seeded.
  - **Groups:** `--groupSize=16` (default) splits the ranks into consecutive groups
    that each run their own collective, **all at the same time**. A **global
    barrier** separates iterations. Each group is a phase `iterN.<type>.gK`.
    `ReportCollectives` prints mean / median / p99 / min / max over the groups.
  - **Training (default workload):**
    - GPipe with PP = 4 stages × DP = 64;
    - rank(s, d) = s·D + d;
    - forward and backward per micro-batch, 8 MB activations between stages;
    - then a per-stage DP ring all-reduce of 64 MB (phases `iterN.allreduce.sK`);
    - then a global barrier.
  - **Placement** `--placement` (rank → server):
    - `strided` (default): rank r on T0 r % 16, so a group of 16 consecutive
      ranks has one GPU per T0 and every ring hop crosses the spine;
    - `packed`: rank r on server r;
    - `random`: `mt19937(1)`.
  - Phase log `logs/*.jct`: `phase,start_us,end_us,duration_us,tasks,done`. The workload starts at t = 2 s.
- **Default sizes** (chosen to be bandwidth-bound, like real gradient sync):
  - `--bytes` = 256 MB per rank (all-reduce, all-to-all, permutation);
  - training: `--grad` = 64 MB, `--act` = 8 MB, `--fwdUs` = 500, `--bwdUs` = 1000, `--mb` = 4;
  - `--iters` = 2.

### Determinism (by design; please keep it)

Every random choice is seeded by a stable identity, so a command always gives
the same result, and different configurations make the same decisions:

- **ECN:** `m_ecnRand.SetStream(SidKey(plane, id))`, plus `SetAntithetic(false)`.
- **Spray labels:** `Hash32(qp id, packet count)`.
- **EV shuffle:** `mt19937(qp id)`.
- **Degraded links:** `mt19937(1)`.
- **Permutation:** seed = iteration × groups + group + 1.
- **QP ids** follow (src, dst) order.

## 2. Decisions the user made (don't undo without asking)

- **SRv6 and ECN:**
  - the LB bit is never set on the last hop (T0 → server); the ECN bit is on
    every port or none (`--lastHopEcn` removed 2026-10-01);
  - with SRv6, switches don't set the ECN bit (`--ecnMark` defaults off), and path
    switching uses the LB bit; congestion control still reads only the ECN bit;
  - ECN is marked at dequeue;
  - ECN uses `m_ecnRand`.
- **Failures** are bandwidth degradation (50 Gbps), not links being cut.
- **EV set:** half the paths active per plane, the same number on every plane,
  random within the plane.
- **Probing:** EV probing is not implemented, for now.
- **No trace generators:** `gen-trace.py` and `FlowV4Id` were deleted; the AI
  workloads are written in ns-3.
- **Scope of edits:** the user sometimes limits edits to specific files.
  Ask when unsure.
- **Fair comparisons:** the user wants the same random decisions in every configuration.
- **Workload shape:**
  - one GPU per server spread over the 4 planes;
  - all-reduce as 16 concurrent 16-rank rings, strided across T0s, rather than one 256-rank ring;
  - long-lived QPs for every pair, built at startup;
  - large buffers;
  - a global barrier between iterations.

## 3. Bugs found and fixed (useful context)

1. **Spurious retransmissions and EV retirement** from a fixed PSN reorder
   threshold under spraying. Replaced by RACK-style time-based detection, window
   widening on duplicate SACKs, and EV restore.
2. **Uninitialised `m_isAntithetic`** in the value-member `UniformRandomVariable`
   used for ECN. It isn't built by CreateObject, so attributes are never applied,
   and results depended on memory layout. Fixed with `SetAntithetic(false)` in
   the `SwitchNode` constructor.
3. **Initial window sized after the first plane only** (about 51 KB instead of
   the 400 Gbps BDP), costing an extra RTT. Now resized while there's no RTT
   sample yet.
4. **Plane registration order.** With QPs created per flow, plane 0 used to send
   before planes 1–3 registered. Moving to long-lived QPs changed permutation
   from 398.2 to 390.2 µs. Restoring the old order reproduced the old run exactly
   (398.208 µs, 543 retransmissions), which proved the new sender and receiver
   match the old behaviour.
5. **Unbalanced random 8-of-16 EV subsets.** Confirmed to be inherent: hash
   spraying over 8 labels shows the same imbalance, so it's not an SRv6 bug.

## 4. Verified results

### (a) The 84-run evaluation (`results/eval-v2/`, `summary_v2.txt`)

This used **older workloads**: one QP per flow, 16 MB, a flat 256-rank ring,
`packed`-like placement. Values are completion times, relative to `spray_trim`.

| workload | finding |
|---|---|
| permutation | per-flow ECMP + PFC is 3.95–4.4× slower (T0→T1 imbalance 4.97). With degraded links (64 / 256), SRv6 half = 0.66× / 0.80×, SRv6 all = 0.64× / 0.69×. |
| alltoall | lossy is +43% on a healthy fabric; trim ≈ PFC. About 32k spurious retransmissions (reordering under incast). |
| allreduce / training | within 1–3% across configs, apart from ECMP + PFC with degraded links (up to +8%). Latency-bound at those sizes. |

### (b) Current code, smaller checks

- **All-reduce, 16 MB, one flat ring:** packed = 1790 µs with 2 channels
  (2046 µs with 1); strided = 3152 µs. Latency-bound, as the model predicts:
  2(d−1)α + 2(d−1)/d · S/B.
- **Groups (2 MB tests, strided, 16 × 16):**

  | pattern | collective mean |
  |---|---|
  | all-reduce | 232 µs |
  | all-to-all | 61 µs |
  | permutation | 61 µs |
  | training DP all-reduce | 701 µs |

- **`results/allreduce-256MB-packed/`** (current defaults, packed): 16
  collectives, mean 10.176 ms (10.174–10.180). That's 95% of the 9.6 ms
  bandwidth bound, and T0→T1 load is 0 (each group sits under one T0).
- **`results/smoke/`:** 1 MB permutation. SRv6 half + trim has a mean
  collective of 42 µs against 105 µs for ECMP + PFC (healthy fabric); 67 against
  142 µs with 64 degraded links.

**Not yet run at the current defaults** (stopped by the user): strided
all-reduce, ECMP + PFC all-reduce, all-to-all, permutation, training.

## 5. Open problems, in suggested priority order

1. **Ring order isn't topology-aware.** The training DP ring (64 ranks, 4 per
   T0) crosses T1 on every hop under strided placement. NCCL would visit a T0's
   GPUs in a row, crossing only 16 times out of 64. Fix: order rings by T0.
2. **The ECMP baseline is too pessimistic.** One flow label per QP means each
   channel is one hashed path per plane, whereas NCCL uses several QPs per
   connection or several UDP source ports. Fix: a setting for entropy values
   (or QPs) per connection.
3. **Everything is synchronised perfectly.** No start or compute jitter, and one
   seed. Add seeded jitter, run several seeds, and report confidence intervals.
   Differences of about 2% are within noise (see bug 4).
4. **Latency model:**
   - all links are 1 µs (real server–T0 cables are about 10–25 ns);
   - switch forwarding takes zero time (real: about 0.3–1 µs);
   - store-and-forward everywhere;
   - NIC and host processing take zero time.
5. **Trimming happens only on buffer overflow.** MRC/UEC trim at a queue-depth
   threshold, so trim runs currently look much like lossy ones.
6. **Failures:** only degradation. There is no link down, flapping or random
   loss, and no EV probing to bring retired EVs back.
7. **The barrier is free:** zero time, no optimizer step. Could become a
   dissemination barrier of log₂N rounds plus `--optimizerUs`.
8. **Training is simplified:** GPipe only (no 1F1B or interleaving), no bucketed
   gradient all-reduce overlapping backward, no TP / FSDP / MoE all-to-all.
9. **Transport gaps:**
   - one SACK per packet, no coalescing;
   - NSCC constants not validated against the UEC reference;
   - no receiver-credit mode (RCCC);
   - no reverse EV set;
   - no tail-loss probes.
10. **Configuration and tooling:**
    - switch buffer and ECN settings aren't configurable at run time;
    - no unit tests;
    - MRC header layouts are unverified;
    - QP memory grows as N², so 1024 servers would need about 13 GB. Could
      allocate QP state on first use.

**Next step the user asked about:** strided all-reduce, comparing spray_trim,
srv6half_trim and srv6all_trim:

```bash
commands/run.sh -t ar-strided -j 3 -d --workload=allreduce --placement=strided \
    --config=spray_trim,srv6half_trim,srv6all_trim
```

## 6. Practical gotchas

- **Don't rebuild while simulations are running:** runs that start later pick up
  the new binary. `run.sh` refuses to build while `ns3.46-pfc-opti` processes exist.
- **Don't use `pkill -f` / `pgrep -f` with patterns** that can match your own
  shell (it killed the shell once, exit 144). Find PIDs with `ps -eo pid,comm`.
- **Detach long runs:** `run.sh -d`, or `setsid nohup … &`. A plain background
  job dies with the session.
- **Large default runs** take about 15–60 minutes and about 1 GB each; the 16-core
  machine handled 6–12 at once. For quick checks use `--bytes=2000000` or
  `16000000`.
- **Checking correctness:** estimate first from first principles (serialization
  at 400 Gbps per GPU, about 2–5 µs per hop) and compare. When a change should
  be neutral, confirm it with a bit-identical rerun.
