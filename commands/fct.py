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
    dfs = dfs[(dfs[4] > 2010000000) & (dfs[4] < 2030000000)]
    dfs.loc[:, 7] = 8000 + dfs[3] * 8 / 400.0
    dfs.loc[:, 8] = dfs[6] / dfs[7]

    df_vec = [
        dfs,
        dfs[dfs[3] < 10000],
        dfs[(dfs[3] < 100000) & (dfs[3] >= 10000)],
        dfs[(dfs[3] < 1000000) & (dfs[3] >= 100000)],
        dfs[dfs[3] >= 1000000],
    ]

    for df in df_vec:
        fctdf = df[6].sort_values()
        slowdf = df[8].sort_values()
        size = len(fctdf)

        print("Flow number: " + str(size))
        print("FCT mean: " + str(fctdf.mean()))
        print("FCT 99%: " + str(fctdf.iloc[int(0.99 * size)]))
        print("FCT 99.9%: " + str(fctdf.iloc[int(0.999 * size)]))
        print("Slowdown mean: " + str(slowdf.mean()))
        print("Slowdown 99%: " + str(slowdf.iloc[int(0.99 * size)]))
        print("Slowdown 99.9%: " + str(slowdf.iloc[int(0.999 * size)]))

    print("Finish FCT")