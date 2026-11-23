#pragma once

/*
 * 本文件声明 WindTunnel 数据集实验入口。
 */

#ifdef __cplusplus
extern "C" {
#endif

// 执行 WindTunnel 数据集的库内实验，并将日志和统计结果写入 experiment_log/。
int run_windtunnel_experiments(void);

#ifdef __cplusplus
}
#endif
