
# InDB-LSS-CDiT

本项目包含两套可独立构建的数据库工程：RemixDB 和 RocksDB 11.8.1。两套工程共用 `remixdb/data/` 数据目录。GitHub 源码仓库不附带数据集或演示样例。完整预处理后的 FLUXNET 和 WindTunnel 数据集通过 GitHub Releases 单独提供。

This repository contains two independently buildable database projects: RemixDB and RocksDB 11.8.1. Both projects use the shared data directory `remixdb/data/`. The GitHub source repository does not bundle dataset files or demo samples. The complete preprocessed FLUXNET and WindTunnel datasets are distributed separately through GitHub Releases.

[GitHub Releases]: https://github.com/username210-debug/InDB-LSS-CDiT/releases

## 1. 数据集下载与可用性

### 1.1 FLUXNET2015（SPACETIME 案例数据，排除 ZA-Kru）

- **案例研究来源：** Mameche, S., Cornanguer, L., Ninad, U., & Vreeken, J. (2025). [SPACETIME: Causal Discovery from Non-Stationary Time Series](https://doi.org/10.1609/aaai.v39i18.34136).
- **案例数据包：** [SPACETIME 作者分享的 Basin 与 FLUXNET 数据包（Google Drive）](https://drive.google.com/file/d/1u8FJah1JolRf8pp_rnAB8u4NTNk8MYMJ/view?usp=sharing)。
- **原始数据集：** [FLUXNET2015](https://fluxnet.org/data/fluxnet2015-dataset/)。
- **数据政策：** [FLUXNET2015 Data Policy](https://fluxnet.org/data/data-policy/)；[2020 年数据政策变更记录](https://fluxnet.org/change-log/)。
- **许可证：** [Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/)，适用于本项目使用的 Tier One 站点数据。
- **下载：** [GitHub Releases]。

SPACETIME 案例数据包列出的 95 个 FLUXNET 站点中，`ZA-Kru` 属于 FLUXNET2015 Tier Two。该站点已从本项目公开数据中排除；其余 94 个站点属于 CC BY 4.0（Tier One）范围。FLUXNET 官方列出的六个 Tier Two 站点为 `RU-Sam`、`RU-SkP`、`RU-Tks`、`RU-Vrk`、`SE-St1` 和 `ZA-Kru`；在上述 95 个案例站点中，只有 `ZA-Kru` 与该名单重合。本项目发布的是该 SPACETIME 案例数据包的 94 个合规站点，不是 FLUXNET2015 全球全部站点。FLUXNET 政策按站点年份（site-year）适用；如果使用其他来源或其他版本的数据，还需按官方政策核对对应站点年份。

从 [GitHub Releases] 下载 FLUXNET 的全部 17 个发布文件：`fluxnet_data.z01` 至 `fluxnet_data.z16`，以及 `fluxnet_data.zip`。将这 17 个文件放在同一目录中，使用 WinRAR 或 7-Zip 解压 `fluxnet_data.zip`，并将解压内容放入 `remixdb/data/`。解压后应得到 `remixdb/data/fluxnet_data/`，其中直接包含 94 个站点目录。每个站点目录保留该案例数据包中可用的 `HH`、`DD`、`WW`、`MM`、`YY` 分辨率的完整预处理文件。发布包包含干净的 `*_pre.csv` 文件以及实验所需的对应故障注入数据，不包含原始 CSV 文件。

如果使用原始 FLUXNET 案例数据重新生成预处理文件，请将站点目录放入 `remixdb/data/fluxnet_data/`，并从原始数据目录中排除整个 `FLX_ZA-Kru_...` 站点目录。然后在 `remixdb/` 目录执行：

```bash
python3 tools/preprocess_fluxnet.py data/fluxnet_data
```

脚本会遍历各站点目录中的原始 CSV，在同一目录生成对应的 `*_pre.csv`；已存在的预处理文件会跳过。脚本也会自动跳过 `ZA-Kru`。预处理过程保留时间戳，按时间稳定排序并去除重复时间戳（同一时间戳保留原文件中最先出现的记录）；将空值、`NA`、`NaN` 和 FLUXNET 缺失标记 `-9999` 视为缺失值；每列最前面的缺失值用该列有效值均值填补，之后的缺失值以前一个有效值前向填充；再使用总体标准差进行 z-score 标准化，并将标准化结果截断到 `[-5, 5]`。

**数据集引用：** Pastorello, G., Trotta, C., Canfora, E., et al. (2020). The FLUXNET2015 dataset and the ONEFlux processing pipeline for eddy covariance data. *Scientific Data*, 7, 225. [https://doi.org/10.1038/s41597-020-0534-3](https://doi.org/10.1038/s41597-020-0534-3)。使用案例数据时也请引用 [SPACETIME (AAAI 2025)](https://doi.org/10.1609/aaai.v39i18.34136)，并按 FLUXNET 政策列明所用站点 ID。

### 1.2 WindTunnel — `wt_walks_v1`

- **原始数据集：** [Causal Chamber `wt_walks_v1`](https://github.com/juangamella/causal-chamber/tree/main/datasets/wt_walks_v1)。
- **随机游走生成脚本：** [actuators_random_walk.py](https://github.com/juangamella/causal-chamber/blob/main/datasets/wt_walks_v1/generators/actuators_random_walk.py)，其中生成 1～16 组随机游走。
- **原始压缩包：** [下载 `wt_walks_v1.zip`](https://causalchamber.s3.eu-central-1.amazonaws.com/downloadables/wt_walks_v1.zip)。
- **官方公布的原始压缩包 MD5：** `19bb4e92cbe0b8dff49299b9b509ac36`。
- **许可证：** [Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/)。上游数据仓库说明其 CSV 文件采用 CC BY 4.0，允许分享和改编，但需要署名并说明修改。
- **下载：** [GitHub Releases]。

从 [GitHub Releases] 下载 `windtunnel_data.zip`，正常解压到 `remixdb/data/`。解压后应得到 `remixdb/data/windtunnel_data/`，所有完整预处理后的 `*_pre.csv` 文件应直接位于该目录中，不要额外嵌套一层 `windtunnel_data/`。

GitHub Releases 中的 `windtunnel_data.zip` 包含完整的 31 个干净预处理 CSV 文件，而不只是当前实验配置选用的两个 regime-jump 文件：16 个 `actuators_random_walk_i_pre.csv`（i=1…16）及其合并文件 `actuators_random_walk_pre.csv`；5 个 `loads_hatch_mix_fast_run_i_pre.csv`（i=1…5）及其合并文件 `loads_hatch_mix_fast_run_pre.csv`；5 个 `loads_hatch_mix_slow_run_i_pre.csv`（i=1…5）及其合并文件 `loads_hatch_mix_slow_run_pre.csv`；以及 `regime_jumps_single_pre.csv`、`regime_jumps_multi_pre.csv`。三个合并文件分别由对应的分段预处理文件按数字顺序拼接而成，表头只保留一次。发布包还包含实验所需的对应故障注入数据，不包含原始 CSV 文件。

如需从原始数据重新生成预处理文件，将原始 CSV 直接放入 `remixdb/data/windtunnel_data/`，在 `remixdb/` 目录执行：

```bash
python3 tools/preprocess_windtunnel.py data/windtunnel_data
```

脚本保留 `timestamp`，对 21 个测量变量进行 z-score 标准化，并将无法解析的数值按 0 处理。完成各分段文件预处理后，如需重新生成三个合并文件，在 `remixdb/data/windtunnel_data/` 目录执行：

```bash
{
  head -n 1 actuators_random_walk_1_pre.csv
  for i in $(seq 1 16); do tail -n +2 "actuators_random_walk_${i}_pre.csv"; done
} > actuators_random_walk_pre.csv

{
  head -n 1 loads_hatch_mix_fast_run_1_pre.csv
  for i in $(seq 1 5); do tail -n +2 "loads_hatch_mix_fast_run_${i}_pre.csv"; done
} > loads_hatch_mix_fast_run_pre.csv

{
  head -n 1 loads_hatch_mix_slow_run_1_pre.csv
  for i in $(seq 1 5); do tail -n +2 "loads_hatch_mix_slow_run_${i}_pre.csv"; done
} > loads_hatch_mix_slow_run_pre.csv
```

**数据集引用：** Gamella, J. L., Peters, J., & Bühlmann, P. (2025). Causal chambers as a real-world physical testbed for AI methodology. *Nature Machine Intelligence*. [https://doi.org/10.1038/s42256-024-00964-x](https://doi.org/10.1038/s42256-024-00964-x)。

### 1.3 不随项目分发的数据集

以下数据集不通过 GitHub Releases 分发；本项目不提供其原始文件、预处理文件、故障文件、真值文件或样例片段。使用者需要通过原始数据提供方或其他合法途径自行取得数据，并遵守相应使用条款。

| 数据集 | 来源与获取入口 | 不分发的原因 |
|---|---|---|
| **Basin** | [Global Runoff Data Centre (GRDC)](https://grdc.bafg.de/data/data_portal/index.html)；亦用于 [SPACETIME](https://doi.org/10.1609/aaai.v39i18.34136)。 | GRDC 对下载的水文数据设置了再分发限制，未经授权不得向公众或第三方（包括通过互联网）重新分发。因此项目不提供 Basin 数据及其衍生文件。 |
| **NETSIM** | [Oxford FMRIB NETSIM](https://www.fmrib.ox.ac.uk/datasets/netsim/index.html)；相关文件也见 [TCDF 的 fMRI 数据目录](https://github.com/M-Nauta/TCDF/tree/master/data/fMRI)。 | 未找到能够明确授权第三方公开再分发该 fMRI 数据的独立许可。TCDF 软件的许可证不自动授予其目录中第三方数据的再分发权。 |
| **DREAM3** | [DREAM Challenge](http://wiki.c2b2.columbia.edu/dream)；[GeneNetWeaver DREAM Challenge 数据归档](https://gnw.sourceforge.net/dreamchallenge.html)。 | 未找到能够明确授权公开再分发 DREAM3 表达数据及真值网络的许可。研究用途的数据获取不等同于公开再分发授权。 |

## 2. 自行取得受限数据集后的预处理

以下命令均从 `remixdb/` 目录执行。RemixDB 与 RocksDB 共用 `remixdb/data/`；RocksDB 从 `../remixdb/data/` 读取数据。

### 2.1 Basin

1. 通过 GRDC 等合法途径取得原始流域数据。每个流域整理为一个 CSV，放入 `remixdb/data/basin_data/`。
2. 每个 CSV 必须包含四列，列顺序为 `time,tavg,prec,Qobs`：日期、平均气温、降水量、实测径流量。若原始下载文件含有其他列，请先按此顺序整理出这四列。
3. 在 `remixdb/` 目录执行：

   ```bash
   python3 tools/preprocess_basin.py data/basin_data
   ```

   脚本为每个原始文件生成同目录的 `<原文件名>_pre.csv`，不会覆盖原始文件。缺失 `tavg` 使用线性插值；缺失 `prec` 填 0；`Qobs` 的开头缺失段填 0、中间缺失段线性插值、末尾缺失段以前一个有效值向前填充；随后对非负 `prec` 和 `Qobs` 执行 `log1p`。该脚本要求上述四列格式，不是通用 GRDC 导出格式转换器。

### 2.2 NETSIM

1. 将 NETSIM 原始时间序列放入 `remixdb/data/netsim/`，文件名按程序约定为 `sim3_subject_0.csv` 至 `sim3_subject_49.csv`。
2. 将对应真值邻接矩阵放在同一目录，命名为 `sim3_subject_0_truth.csv` 至 `sim3_subject_49_truth.csv`。
3. NETSIM 的预处理已经实现于 `remixdb/experiment/cdit_experiment.cpp`。调用 `run_cdit_experiments()` 时，程序会在 `_pre.csv` 不存在时自动生成预处理文件：对每一行执行 z-score 标准化，并写出 `sim3_subject_N_pre.csv`。
4. 根据实际运行需要，直接编辑 `remixdb/experiment/netism_cdit_experiment_input.csv` 中的试验品集合、最大滞后和显著性水平。项目已保留完整实验配置，无需从其他目录复制。

### 2.3 DREAM3

1. 将原始表达矩阵放入 `remixdb/data/dream3/Dream3TensorData/`。程序使用的文件名为 `Size10Ecoli1.csv`、`Size10Ecoli2.csv`、`Size10Yeast1.csv`、`Size10Yeast2.csv`、`Size10Yeast3.csv`，以及对应的 Size50 和 Size100 文件。
2. 将真值调控网络放入 `remixdb/data/dream3/TrueGeneNetworks/`，文件名按程序约定，例如 `InSilicoSize10-Ecoli1.txt`、`InSilicoSize10-Yeast1.txt`，并包含相应 Size50、Size100 文件。
3. DREAM3 的预处理已经实现于 `remixdb/experiment/cdit_experiment.cpp`。调用 `run_cdit_experiments()` 时，程序会在 `_pre.csv` 不存在时自动生成预处理文件：对表达矩阵的每一列执行 z-score 标准化。
4. 根据实际运行需要，直接编辑 `remixdb/experiment/dream3_cdit_experiment_input.csv` 中的实验对象、最大滞后和显著性水平。项目已保留完整实验配置，无需从其他目录复制。

NETSIM 和 DREAM3 的预处理由 `run_cdit_experiments()` 自动触发；该入口会依次执行两个数据集的实验。项目随附的两个输入 CSV 均保留完整原始配置。启用该入口前，需要分别按照上述说明准备好 NETSIM 和 DREAM3 的数据。

## 3. 生成真实数据集的故障片段

RemixDB 的 `realdataset_fault_generation` 工具支持 **Basin、FLUXNET 和 WindTunnel**，不支持 NETSIM 或 DREAM3。GitHub Releases 已包含 FLUXNET 和 WindTunnel 实验所需的预处理数据及对应故障注入数据。对于默认实验，用户下载并解压数据集后即可运行，无需再次执行故障生成工具。该工具仍然保留，可根据需要从干净的 `*_pre.csv` 文件在本地重新生成故障片段和延迟修复片段。

1. 准备相应数据集的干净预处理文件：FLUXNET 和 WindTunnel 从 GitHub Releases 下载；Basin 按第 2.1 节完成预处理。
2. 打开 `remixdb/experiment/realdataset_fault_generation_input.csv`，保留需要生成故障数据的数据集配置行，并按需调整故障比例、故障长度和修复延迟。
3. 打开 `remixdb/xdbdemo.c`，在 `main()` 中注释默认的 `return run_multiple_experiments();`，取消注释 `return run_realdataset_fault_generation_experiments();`。
4. 在 `remixdb/` 目录编译并运行：

   ```bash
   make CCC=gcc XCC=g++ xdbdemo.out
   ./xdbdemo.out
   ```

5. 生成完成后，恢复 `main()` 中的默认入口并重新编译。若要在实验中使用故障文件，将相应实验输入 CSV 中的 `是否测试故障数据集` 设置为 `是`。

每次运行故障生成工具时，程序会先清理 `basin_data/`、`fluxnet_data/` 和 `windtunnel_data/` 下此前生成的故障版本，再根据当前配置重新生成；干净的 `*_pre.csv` 源文件会保留。FLUXNET 的 `ZA-Kru` 站点会被跳过，不会生成故障版本。该工具属于 RemixDB，RocksDB 没有单独的故障生成入口。

## 4. 实验入口与默认运行内容

默认情况下，RemixDB 和 RocksDB 均运行 **FLUXNET（不含 ZA-Kru）与 WindTunnel** 的实验。对应的 FLUXNET 和 WindTunnel 实验输入 CSV 已放在各自 `experiment/` 目录中并启用。

### RemixDB — `remixdb/xdbdemo.c`

- **FLUXNET：** 默认启用 `run_fluxnet_experiments()` 和 `run_outdb_fluxnet_experiments()`。
- **WindTunnel：** 默认启用 `run_windtunnel_experiments()` 和 `run_outdb_windtunnel_experiments()`。
- **Basin：** 取得数据并完成预处理后，使用已保留在 `remixdb/experiment/` 中的 Basin 输入配置；完成数据准备和预处理后，按需调整参数并取消对应 Basin 调用的注释。
- **NETSIM / DREAM3：** 准备好两个数据集的数据后，按需调整各自的输入 CSV，取消 `run_cdit_experiments()` 的注释。该入口会依次执行 NETSIM 和 DREAM3 实验。

### RocksDB — `rocksdb/rdbdemo.c`

- **FLUXNET：** 默认启用 `run_fluxnet_experiments()` 和 `run_outdb_fluxnet_experiments()`。
- **WindTunnel：** 默认启用 `run_windtunnel_experiments()` 和 `run_outdb_windtunnel_experiments()`。
- **Basin：** 取得数据并完成预处理后，使用已保留在 `rocksdb/experiment/` 中的 Basin 输入配置；完成数据准备和预处理后，按需调整参数并取消对应 Basin 调用的注释。

RocksDB 工程不包含 NETSIM/DREAM3 基线入口，也不包含 RemixDB 的真实数据故障生成工具。所有实验输入 CSV 均保留在各自的 `experiment/` 目录中。

## 5. 编译与运行

### RemixDB（CLion 或终端）

在 CLion 中打开 `remixdb/`，并将 `remixdb/` 设置为工作目录。也可以在终端执行：

```bash
cd remixdb
make CCC=gcc XCC=g++ xdbdemo.out
./xdbdemo.out
```

若使用 CLion 的运行按钮，将 `make xdbdemo.out` 配置为构建目标，将 `xdbdemo.out` 配置为可执行文件，并将工作目录设为 `remixdb/`。

### RocksDB 11.8.1（VS Code 或终端）

在 VS Code 中打开 `rocksdb/` 作为工作区，选择 **Run and Debug → RocksDB experiment**，或在终端执行：

```bash
cd rocksdb
make -f Makefile.causal -j2 rdbdemo
./rdbdemo
```

`Makefile.causal` 会先构建 RocksDB 静态库，再构建实验可执行文件。RocksDB 从 `../remixdb/data/` 读取共享数据集。

### Ubuntu 编译依赖

```bash
sudo apt update
sudo apt install build-essential gdb cmake gcc-11 g++-11 \
  libgflags-dev libsnappy-dev zlib1g-dev libbz2-dev \
  liblz4-dev libzstd-dev python3
```

实验日志分别写入 RemixDB 和 RocksDB 的 `experiment_log/` 目录。运行时数据库文件、元数据和日志会在运行过程中生成。

## 6. 软件许可证与第三方数据集声明

RemixDB 和 RocksDB 保留各自上游许可证，二者许可证相互独立。第三方数据集的来源、许可证、署名和修改说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

---

# InDB-LSS-CDiT (English)

This repository contains the complete RemixDB and RocksDB 11.8.1 source projects. Both projects use the shared `remixdb/data/` directory. The GitHub repository does not bundle dataset files or demo samples. The complete preprocessed FLUXNET and WindTunnel datasets are distributed separately through GitHub Releases.

## Dataset downloads

### FLUXNET2015 — SPACETIME case-study data (excluding ZA-Kru)

- Case-study paper: Mameche, S., Cornanguer, L., Ninad, U., & Vreeken, J. (2025), [SPACETIME: Causal Discovery from Non-Stationary Time Series](https://doi.org/10.1609/aaai.v39i18.34136).
- Case-study data package shared by the SPACETIME authors: [Google Drive archive](https://drive.google.com/file/d/1u8FJah1JolRf8pp_rnAB8u4NTNk8MYMJ/view?usp=sharing).
- Original dataset: [FLUXNET2015](https://fluxnet.org/data/fluxnet2015-dataset/).
- Policy: [FLUXNET2015 Data Policy](https://fluxnet.org/data/data-policy/) and [Data Change Log](https://fluxnet.org/change-log/).
- License for the included Tier One sites: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
- Download: [GitHub Releases].

The SPACETIME case-study package lists 95 FLUXNET site directories. `ZA-Kru` is a Tier Two site and is excluded. The remaining 94 sites fall under the CC BY 4.0 (Tier One) policy. FLUXNET lists six Tier Two sites: `RU-Sam`, `RU-SkP`, `RU-Tks`, `RU-Vrk`, `SE-St1`, and `ZA-Kru`; only `ZA-Kru` occurs in the 95-site SPACETIME package. This release covers the 94 eligible sites in the SPACETIME case-study package, not all global FLUXNET2015 sites. The FLUXNET policy applies at the site-year level; other FLUXNET copies or releases must be checked against the applicable policy for each site-year.

Download all 17 FLUXNET release assets from [GitHub Releases]: `fluxnet_data.z01` through `fluxnet_data.z16`, and `fluxnet_data.zip`. Place all 17 files in the same directory. Extract `fluxnet_data.zip` using WinRAR or 7-Zip, and place the extracted contents in `remixdb/data/`. The resulting directory must be `remixdb/data/fluxnet_data/`, containing the 94 site directories. Each site directory retains all available preprocessed resolutions (`HH`, `DD`, `WW`, `MM`, and `YY`) from the case-study package. The release archive contains clean `*_pre.csv` files and the corresponding fault-injected data required by the experiments, but does not include raw CSV files.

To regenerate the preprocessed files from the original case-study data, place the site directories under `remixdb/data/fluxnet_data/`, excluding the entire `FLX_ZA-Kru_...` directory, and run this command from `remixdb/`:

```bash
python3 tools/preprocess_fluxnet.py data/fluxnet_data
```

The script processes raw CSV files in each site directory and writes corresponding `*_pre.csv` files beside them. Existing preprocessed files are skipped. The script also automatically skips `ZA-Kru`. Preprocessing preserves timestamps, stably sorts records by time, removes duplicate timestamps while retaining the first source record, treats blank values, `NA`, `NaN`, and `-9999` as missing, fills initial missing values with the column mean and later missing values by forward filling, applies z-score normalization using the population standard deviation, and clips normalized values to `[-5, 5]`.

**Dataset citations:** Pastorello, G., Trotta, C., Canfora, E., et al. (2020). “The FLUXNET2015 dataset and the ONEFlux processing pipeline for eddy covariance data.” *Scientific Data*, 7, 225. https://doi.org/10.1038/s41597-020-0534-3. Please also cite [SPACETIME (AAAI 2025)](https://doi.org/10.1609/aaai.v39i18.34136) when using the case-study data, and list the site IDs used as required by the FLUXNET policy.

### WindTunnel — `wt_walks_v1`

- Original dataset: [Causal Chamber `wt_walks_v1`](https://github.com/juangamella/causal-chamber/tree/main/datasets/wt_walks_v1).
- Random-walk generator: [actuators_random_walk.py](https://github.com/juangamella/causal-chamber/blob/main/datasets/wt_walks_v1/generators/actuators_random_walk.py), which generates 16 random-walk runs.
- Original archive: [Download `wt_walks_v1.zip`](https://causalchamber.s3.eu-central-1.amazonaws.com/downloadables/wt_walks_v1.zip).
- Published original archive MD5: `19bb4e92cbe0b8dff49299b9b509ac36`.
- License: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), as stated by the upstream repository for its CSV files.
- Download: [GitHub Releases].

Download `windtunnel_data.zip` from [GitHub Releases] and extract it into `remixdb/data/`. The resulting directory must be `remixdb/data/windtunnel_data/`, with all clean `*_pre.csv` files directly inside it. Do not create an extra nested `windtunnel_data/` directory.

The `windtunnel_data.zip` release archive contains the complete set of 31 clean preprocessed CSV files, not just the two regime-jump files selected by the default experiment configuration: 16 individual random-walk files plus their concatenated file, five individual fast-run files plus their concatenated file, five individual slow-run files plus their concatenated file, and the two regime-jump files. Each concatenated file contains one header followed by the data rows from its component files in numeric order. The archive also contains the corresponding fault-injected data required by the experiments, but does not include raw CSV files.

To regenerate the preprocessed files from the original raw archive, put the raw CSV files directly in `remixdb/data/windtunnel_data/` and run from `remixdb/`:

```bash
python3 tools/preprocess_windtunnel.py data/windtunnel_data
```

The script preserves `timestamp`, z-scores the 21 measured variables, and treats values that cannot be parsed as numeric as zero. After preprocessing, create the three concatenated files from their component files in numeric order, keeping the header only once. For example, from `remixdb/data/windtunnel_data/`:

```bash
{
  head -n 1 actuators_random_walk_1_pre.csv
  for i in $(seq 1 16); do tail -n +2 "actuators_random_walk_${i}_pre.csv"; done
} > actuators_random_walk_pre.csv

{
  head -n 1 loads_hatch_mix_fast_run_1_pre.csv
  for i in $(seq 1 5); do tail -n +2 "loads_hatch_mix_fast_run_${i}_pre.csv"; done
} > loads_hatch_mix_fast_run_pre.csv

{
  head -n 1 loads_hatch_mix_slow_run_1_pre.csv
  for i in $(seq 1 5); do tail -n +2 "loads_hatch_mix_slow_run_${i}_pre.csv"; done
} > loads_hatch_mix_slow_run_pre.csv
```

**Citation:** Gamella, J. L., Peters, J., & Bühlmann, P. (2025). “Causal chambers as a real-world physical testbed for AI methodology.” *Nature Machine Intelligence*. https://doi.org/10.1038/s42256-024-00964-x.

### Datasets not distributed with this project

No raw, preprocessed, faulted, ground-truth, or sample files are distributed for the following datasets:

- **Basin:** sourced from the Global Runoff Data Centre (GRDC) and used in the SPACETIME case study. GRDC restricts redistribution of downloaded hydrological data to unauthorized third parties or the public, including online distribution. Source: [GRDC Data Portal](https://grdc.bafg.de/data/data_portal/index.html).
- **NETSIM:** sourced from [Oxford FMRIB NETSIM](https://www.fmrib.ox.ac.uk/datasets/netsim/index.html); related files are referenced in the [TCDF fMRI data directory](https://github.com/M-Nauta/TCDF/tree/master/data/fMRI). No dataset-specific public redistribution permission was identified for the third-party fMRI data.
- **DREAM3:** sourced from the [DREAM Challenge](http://wiki.c2b2.columbia.edu/dream) and [GeneNetWeaver archive](https://gnw.sourceforge.net/dreamchallenge.html). No clear public redistribution permission was identified for the expression data or ground-truth networks.

## Preparing separately obtained datasets

Run the following commands from `remixdb/`. RocksDB reads the same shared data directory through `../remixdb/data/`.

### Basin

1. Obtain the Basin data through a lawful access route. Place one CSV per catchment in `remixdb/data/basin_data/`.
2. Each CSV must contain these four columns, in this order: `time,tavg,prec,Qobs`. Arrange the downloaded records into this format before preprocessing.
3. Run:

   ```bash
   python3 tools/preprocess_basin.py data/basin_data
   ```

   The script writes `<original-name>_pre.csv` beside each raw file. It linearly interpolates missing `tavg`; fills missing `prec` with zero; fills the `Qobs` prefix with zero, interpolates internal gaps, and forward-fills the suffix; then applies `log1p` to non-negative `prec` and `Qobs` values.

### NETSIM

1. Put the raw time-series files `sim3_subject_0.csv` through `sim3_subject_49.csv` in `remixdb/data/netsim/`.
2. Put the corresponding ground-truth adjacency matrices `sim3_subject_0_truth.csv` through `sim3_subject_49_truth.csv` in the same directory.
3. NETSIM preprocessing is implemented in `remixdb/experiment/cdit_experiment.cpp`. Calling `run_cdit_experiments()` automatically creates `sim3_subject_N_pre.csv` when the file does not exist, using row-wise z-score normalization.
4. Edit `remixdb/experiment/netism_cdit_experiment_input.csv` to configure the subjects, maximum lag, and significance level. The complete original experiment configuration is already included; no copying from another directory is required.

### DREAM3

1. Put the raw expression matrices in `remixdb/data/dream3/Dream3TensorData/`. The expected names include `Size10Ecoli1.csv`, `Size10Ecoli2.csv`, `Size10Yeast1.csv`, `Size10Yeast2.csv`, `Size10Yeast3.csv`, and the corresponding Size50 and Size100 files.
2. Put the ground-truth regulatory networks in `remixdb/data/dream3/TrueGeneNetworks/`, using names such as `InSilicoSize10-Ecoli1.txt` and `InSilicoSize10-Yeast1.txt`, plus the corresponding Size50 and Size100 files.
3. DREAM3 preprocessing is implemented in `remixdb/experiment/cdit_experiment.cpp`. Calling `run_cdit_experiments()` automatically creates the `_pre.csv` matrices when missing, using column-wise z-score normalization.
4. Edit `remixdb/experiment/dream3_cdit_experiment_input.csv` to configure the subjects, maximum lag, and significance level. The complete original experiment configuration is already included; no copying from another directory is required.

The `run_cdit_experiments()` entry point executes both NETSIM and DREAM3 experiments. Both complete original input configurations are included in the repository. Prepare both datasets as described above before enabling this entry point.

## Generating faulted real-dataset files

The RemixDB `realdataset_fault_generation` utility supports **Basin, FLUXNET, and WindTunnel**; it does not support NETSIM or DREAM3. The GitHub Releases archives already contain the preprocessed FLUXNET and WindTunnel data and the corresponding fault-injected data required by the experiments. Users do not need to run this utility to reproduce the default experiments after downloading and extracting the release archives. The utility is retained for regenerating faulted and delayed-repair variants locally from clean `*_pre.csv` files when needed.

1. Prepare clean preprocessed files: download FLUXNET or WindTunnel from GitHub Releases, or preprocess Basin as described above.
2. Edit `remixdb/experiment/realdataset_fault_generation_input.csv`, keeping the dataset rows you want and adjusting fault ratios, fault lengths, and repair delays as needed.
3. In `remixdb/xdbdemo.c`, comment out `return run_multiple_experiments();` in `main()` and uncomment `return run_realdataset_fault_generation_experiments();`.
4. Build and run from `remixdb/`:

   ```bash
   make CCC=gcc XCC=g++ xdbdemo.out
   ./xdbdemo.out
   ```

5. Restore the default `main()` entry point and rebuild. To use the generated files in an experiment, set `是否测试故障数据集` to `是` in that experiment's input CSV.

Each fault-generation run first removes previously generated variants under `basin_data/`, `fluxnet_data/`, and `windtunnel_data/`, then regenerates them from the clean `*_pre.csv` files. The clean source files are retained. `ZA-Kru` is skipped. Fault generation is provided by RemixDB; RocksDB has no separate fault-generation entry point.

## Default experiments and optional experiment entry points

By default, both RemixDB and RocksDB run **FLUXNET (excluding ZA-Kru) and WindTunnel** experiments. Their active experiment input CSV files are already present in each project's `experiment/` directory.

### RemixDB — `remixdb/xdbdemo.c`

- **FLUXNET:** `run_fluxnet_experiments()` and `run_outdb_fluxnet_experiments()` are enabled by default.
- **WindTunnel:** `run_windtunnel_experiments()` and `run_outdb_windtunnel_experiments()` are enabled by default.
- **Basin:** use the Basin input CSV files already present in `remixdb/experiment/`; after preparing and preprocessing the data, adjust parameters as needed and uncomment the corresponding Basin calls.
- **NETSIM / DREAM3:** after preparing both datasets, adjust their input CSV files as needed and uncomment `run_cdit_experiments()`. This entry point executes both NETSIM and DREAM3 experiments.

### RocksDB — `rocksdb/rdbdemo.c`

- **FLUXNET:** `run_fluxnet_experiments()` and `run_outdb_fluxnet_experiments()` are enabled by default.
- **WindTunnel:** `run_windtunnel_experiments()` and `run_outdb_windtunnel_experiments()` are enabled by default.
- **Basin:** use the Basin input CSV files already present in `rocksdb/experiment/`; after preparing and preprocessing the data, adjust parameters as needed and uncomment the corresponding Basin calls.

RocksDB does not include the NETSIM/DREAM3 baseline entry point or the RemixDB fault-generation utility. All experiment input CSV files remain in the corresponding `experiment/` directories.

## Build and run

RemixDB:

```bash
cd remixdb
make CCC=gcc XCC=g++ xdbdemo.out
./xdbdemo.out
```

RocksDB 11.8.1:

```bash
cd rocksdb
make -f Makefile.causal -j2 rdbdemo
./rdbdemo
```

On Ubuntu, install the required build dependencies with:

```bash
sudo apt update
sudo apt install build-essential gdb cmake gcc-11 g++-11 \
  libgflags-dev libsnappy-dev zlib1g-dev libbz2-dev \
  liblz4-dev libzstd-dev python3
```

Experiment logs are written to the respective `experiment_log/` directories. Runtime database files and metadata are generated when the experiments run. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for software licenses and dataset attribution details.
