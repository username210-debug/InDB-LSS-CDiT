#pragma once

/*
 * 本文件声明 Fluxnet 数据集库外处理对照实验入口。
 */


// 从 rocksdb/experiment/outdb_fluxnet_experiment_input.csv 逐行执行
// 生物圈大气通量“入库 + RocksDB 库外 CDiT/LSS-CDiT 推断”实验。
// 返回 0 表示全部实验成功；非 0 表示至少一项实验失败。
int run_outdb_fluxnet_experiments();
