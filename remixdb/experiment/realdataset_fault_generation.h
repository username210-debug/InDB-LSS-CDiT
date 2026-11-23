#ifndef REALDATASET_FAULT_GENERATION_H
#define REALDATASET_FAULT_GENERATION_H

/*
 * 本文件声明真实数据集故障片段生成实验的统一入口。
 */

#ifdef __cplusplus
extern "C" {
#endif

// 按配置批量生成真实数据集的故障片段及后续修复数据。
int run_realdataset_fault_generation_experiments(void);

#ifdef __cplusplus
}
#endif

#endif
