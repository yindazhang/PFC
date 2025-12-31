import math
from optparse import OptionParser

loads = [0.3, 0.4, 0.5, 0.6, 0.7]

pfc_version = [0, 1]
cc_version = [0, 1]


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
            cmd += "--flow=" + dataset + "_320_" + str(load) + "_100G_" + duration
            cmd += '" > '
            print(cmd + outFile + "-" + str(load) + "-" + dataset + ".out &")
        print()
    print()

def AddCC(start, outFile, pfc):
    for i in cc_version:
        if pfc == 0 and i == 0:
            continue
        cmd = start
        cmd += "--cc=" + str(i) + " "
        AddLoad(cmd, outFile + "-CC" + str(i))

def AddPFC(start, outFile):
    for pfc in pfc_version:
        cmd = start
        cmd += "--pfc=" + str(pfc) + " "
        AddCC(cmd, outFile + "PFC" + str(pfc), pfc)

if __name__ == "__main__":
    start = 'nohup ./ns3 run "scratch/pfc '
    outFile = ""
    AddPFC(start, outFile)