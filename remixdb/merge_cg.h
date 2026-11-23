#pragma once

/*
 * 本文件声明相邻窗口因果图的合并结果结构及合并接口。
 */

#include "cdit_module.h"

// 相邻窗口图合并的判定结果及合并后的边-滞后集合。
struct MergeCGResult {
    int merge = 0;
    CDiTResult merged;
    double similarity = 0.0;
    double sim_lag = 0.0;
};

// 比较两个相邻窗口的图结构与滞后集合；相似度达到 theta 时返回合并结果。
MergeCGResult merge_adjacent_causal_graphs(
        const CDiTResult& C1,
        const CDiTResult& C2,
        int tau,
        int Delta,
        double theta,
        bool ignore_self
);
