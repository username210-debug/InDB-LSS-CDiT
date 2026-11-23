#pragma once

/*
 * 本文件声明 WindTunnel 数据集实验入口。调用方应先准备好对应的数据目录和输入配置文件。
 */


// 运行风洞真实数据集上的 RocksDB 库内 CDiT / LSS-CDiT 实验。
// 返回 0 表示全部实验成功，非 0 表示实验失败。
int run_windtunnel_experiments();
