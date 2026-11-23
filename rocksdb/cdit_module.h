#pragma once

/*
 * 本文件声明窗口级分析结果结构及原始计算路径、统计量缓存路径的统一接口。输入数据按“时间 × 变量”组织。
 */

#include <map>
#include <string>
#include <utility>
#include <vector>

// 一条有向边以 (原因变量编号, 结果变量编号) 为键；值保存该边对应的全部直接因果滞后。
struct CDiTResult {
    std::map<std::pair<int,int>, std::vector<int>> lags_by_edge;
};

// 原始 CDiT 实现。默认的 run_cdit(...) 仍调用该实现，
// 以保持现有数据库内元数据代码的调用行为不变。
// 参考实现：按原有方式逐次构造检验所需数据并执行条件独立性检验。
CDiTResult run_cdit_original(const std::vector<std::vector<double>>& data,
                             int tau,
                             double alpha,
                             bool infer_self = true,
                             const std::vector<int>* source_columns = nullptr,
                             const std::vector<int>* target_columns = nullptr);

// 基于协方差元数据的 CDiT。其输入输出接口与
// run_cdit_original(...) 一致，但通过复用窗口内的滞后协方差元数据，
// 减少重复 PaCoT 偏相关检验中的矩阵计算。
// 统计量复用实现：从窗口级滞后协方差缓存提取检验子矩阵。
CDiTResult run_cdit_covmeta(const std::vector<std::vector<double>>& data,
                            int tau,
                            double alpha,
                            bool infer_self = true,
                            const std::vector<int>* source_columns = nullptr,
                            const std::vector<int>* target_columns = nullptr);

// 兼容旧接口的默认入口：原始 CDiT 实现。
// 兼容入口：保持原有调用接口和默认计算路径。
CDiTResult run_cdit(const std::vector<std::vector<double>>& data,
                    int tau,
                    double alpha,
                    bool infer_self = true,
                    const std::vector<int>* source_columns = nullptr,
                    const std::vector<int>* target_columns = nullptr);

// 以下函数将边及其滞后集合转换为便于日志输出的文本。
std::string format_inferred(const CDiTResult& r);
std::string format_inferred_pretty(const CDiTResult& r);
std::string format_abstract_graph(const CDiTResult& r);
std::string format_inferred_named(const CDiTResult& r,
                                  const std::vector<std::string>& names);
