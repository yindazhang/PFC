import math
from optparse import OptionParser

loads = [0.5, 0.7]

pfc_version = [1]
cc_version = [1, 3]
mtu_version = [1000, 2000, 4000, 8000]


datasets = ["Storage"]
durations = ["0.2"]
#datasets = ["Storage", "WebSearch", "Cache", "Hadoop"]
#durations = ["0.2", "0.2", "0.2", "0.2"]

def AddLoad(start, outFile):
    global hG
    arr = loads
    for index in range(len(datasets)):
        dataset = datasets[index]
        duration = durations[index]
        for load in loads:
            cmd = start
            cmd += "--time=" + duration + " "
            cmd += " "
            cmd += "--flow=" + dataset + "_64_" + str(load) + "_400G_" + duration
            cmd += '" > '
            print(cmd + outFile + "-" + str(load) + "-" + dataset + ".out &")
        print()
    print()

def AddMTU(start, outFile):
    for mtu in mtu_version:
        cmd = start
        cmd += "--mtu=" + str(mtu) + " "
        AddLoad(cmd, outFile + "-MTU" + str(mtu))

def AddCC(start, outFile):
    for i in cc_version:
        cmd = start
        cmd += "--cc=" + str(i) + " "
        AddMTU(cmd, outFile + "-CC" + str(i))

def AddPFC(start, outFile):
    for pfc in pfc_version:
        cmd = start
        cmd += "--pfc=" + str(pfc) + " "
        AddCC(cmd, outFile + "PFC" + str(pfc))

if __name__ == "__main__":
    start = 'nohup ./ns3 run "scratch/pfc '
    outFile = ""
    AddPFC(start, outFile)