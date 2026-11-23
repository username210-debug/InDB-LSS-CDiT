#pragma once

/*
 * 本文件声明 Fluxnet 数据集实验入口。调用方应先准备好对应的数据目录和输入配置文件。
 */


// 从 rocksdb/experiment/fluxnet_experiment_input.csv 逐行执行生物圈-大气通量库内实验。
// 返回 0 表示全部实验成功；非 0 表示至少一项实验失败。
int run_fluxnet_experiments();
