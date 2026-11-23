/*
 * 本文件是 RocksDB 实验程序的统一入口；该文件以 C++ 模式编译。
 * 默认执行可公开获取的 FLUXNET2015 Tier One（排除 ZA-Kru）和 WindTunnel 实验。
 * Basin 的实验入口保留，准备好本地数据后可按根目录 README 启用。
 */

#include "experiment/basin_experiment.h"
#include "experiment/fluxnet_experiment.h"
#include "experiment/windtunnel_experiment.h"
#include "experiment/outdb_basin_experiment.h"
#include "experiment/outdb_fluxnet_experiment.h"
#include "experiment/outdb_windtunnel_experiment.h"

int run_multiple_experiments()
{
    int rc;

    /* Basin：取得原始数据并完成预处理后，可启用相应实验。 */
    // rc = run_basin_experiments();
    // if (rc != 0) return rc;
    // rc = run_outdb_basin_experiments();
    // if (rc != 0) return rc;

    /* 默认启用：FLUXNET2015 Tier One 站点（不含 ZA-Kru）。 */
    rc = run_fluxnet_experiments();
    if (rc != 0) return rc;
    rc = run_outdb_fluxnet_experiments();
    if (rc != 0) return rc;

    /* 默认启用：Causal Chamber wt_walks_v1。 */
    rc = run_windtunnel_experiments();
    if (rc != 0) return rc;

    rc = run_outdb_windtunnel_experiments();
    if (rc != 0) return rc;

    return 0;
}

int main()
{
    /* 单独运行某项实验时，可在此处选择对应入口。 */
    // return run_basin_experiments();
    // return run_fluxnet_experiments();
    // return run_windtunnel_experiments();
    // return run_outdb_basin_experiments();
    // return run_outdb_fluxnet_experiments();
    // return run_outdb_windtunnel_experiments();

    return run_multiple_experiments();
}
