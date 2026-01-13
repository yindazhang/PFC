import argparse
import subprocess

import numpy as np
import pandas as pd

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="")
    parser.add_argument("-f", dest="file", action="store", help="Specify the fct file.")
    args = parser.parse_args()

    names = []
    stats = ["Number", "Sum", "Mean", "99%", "99.9%"]

    fct_file = args.file
    dfs = pd.read_csv(fct_file, header=None)
    dfs = dfs[(dfs[4] > 2040000000) & (dfs[4] < 2160000000)]

    df_vec = [
        dfs,
        dfs[dfs[3] < 10000],
        dfs[(dfs[3] < 100000) & (dfs[3] >= 10000)],
        dfs[(dfs[3] < 1000000) & (dfs[3] >= 100000)],
        dfs[dfs[3] >= 1000000],
    ]

    for df in df_vec:
        tmpdf = df[6].sort_values()
        size = len(tmpdf)

        print("Flow number: " + str(size))
        print("FCT mean: " + str(tmpdf.mean()))
        print("FCT 99%: " + str(tmpdf.iloc[int(0.99 * size)]))
        print("FCT 99.9%: " + str(tmpdf.iloc[int(0.999 * size)]))

    print("Finish FCT")