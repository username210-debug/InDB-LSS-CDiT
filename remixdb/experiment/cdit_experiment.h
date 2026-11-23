#pragma once

/*
 * 本文件声明 NETSIM 与 DREAM3 数据集基线实验入口。
 */

#ifdef __cplusplus
extern "C" {
#endif

// 执行 NETSIM 与 DREAM3 的库外基线实验，并计算相应评价指标。
int run_cdit_experiments(void);

#ifdef __cplusplus
}
#endif
