
# InDB-LSS-CDiT

本项目包含两套可独立构建的数据库工程：RemixDB 和 RocksDB 11.8.1。两套工程共用 `remixdb/data/` 数据目录。GitHub 源码仓库不附带数据集或演示样例。完整预处理后的 FLUXNET 和 WindTunnel 数据集（包括干净数据和故障注入版本）通过 GitHub Releases 单独提供。

This repository contains two independently buildable database projects: RemixDB and RocksDB 11.8.1. Both projects use the shared data directory `remixdb/data/`. The GitHub source repository does not bundle dataset files or demo samples. The complete preprocessed FLUXNET and WindTunnel datasets, including clean data and fault-injected variants, are distributed separately through GitHub Releases.

[GitHub Releases]: https://github.com/username210-debug/InDB-LSS-CDiT/releases

## 1. 数据集下载与可用性

本项目通过 GitHub Releases 完整公开以下两个数据集：

- FLUXNET2015：SPACETIME 案例数据中的94个合规站点，排除 ZA-Kru。
- WindTunnel：Causal Chamber 的完整 `wt_walks_v1` 数据集。

两个数据集的压缩包均包含预处理后的干净数据和对应的故障注入版本。使用者下载并解压后即可运行实验，无需另外执行故障生成程序。

**文件命名说明：**

文件名中的 `_pre.csv` 表示经过本项目预处理后的干净数据文件。例如：

`actuators_random_walk_1_pre.csv`

故障注入文件以对应的 `_pre.csv` 文件为基础生成，文件名会在 `_pre` 后追加故障参数，例如：

`actuators_random_walk_1_pre_3%_[20,1500]_[50,2500].csv`

其中包含故障比例、故障片段长度范围和修复延迟范围等信息。

### 1.1 FLUXNET2015（SPACETIME 案例数据，排除 ZA-Kru）

**数据来源与许可证**

- **案例研究来源：** Mameche, S., Cornanguer, L., Ninad, U., & Vreeken, J. (2025). [SPACETIME: Causal Discovery from Non-Stationary Time Series](https://doi.org/10.1609/aaai.v39i18.34136).
- **案例数据包：** [SPACETIME 作者分享的 Basin 与 FLUXNET 数据包（Google Drive）](https://drive.google.com/file/d/1u8FJah1JolRf8pp_rnAB8u4NTNk8MYMJ/view?usp=sharing)。
- **原始数据集：** [FLUXNET2015](https://fluxnet.org/data/fluxnet2015-dataset/)。
- **数据政策：** [FLUXNET2015 Data Policy](https://fluxnet.org/data/data-policy/)；[2020 年数据政策变更记录](https://fluxnet.org/change-log/)。
- **许可证：** [Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/)，适用于本项目使用的 Tier One 站点数据。
- **下载入口：** [GitHub Releases]。

SPACETIME 案例数据包列出的95个 FLUXNET 站点中，`ZA-Kru` 属于 FLUXNET2015 Tier Two。该站点已从本项目公开数据中排除；其余94个站点属于 CC BY 4.0（Tier One）范围。FLUXNET 官方列出的六个 Tier Two 站点为 `RU-Sam`、`RU-SkP`、`RU-Tks`、`RU-Vrk`、`SE-St1` 和 `ZA-Kru`；在上述95个案例站点中，只有 `ZA-Kru` 与该名单重合。

本项目发布的是该 SPACETIME 案例数据包的94个合规站点，不是 FLUXNET2015 全球全部站点。FLUXNET 政策按站点年份（site-year）适用；如果使用其他来源或其他版本的数据，还需按官方政策核对对应站点年份。

**下载与解压**

从 [GitHub Releases] 下载 FLUXNET 数据集的全部5个分卷文件：

- `fluxnet_data.z01`
- `fluxnet_data.z02`
- `fluxnet_data.z03`
- `fluxnet_data.z04`
- `fluxnet_data.zip`

将全部5个文件放在同一个目录中，使用 WinRAR 或 7-Zip 解压 `fluxnet_data.zip`，并将解压得到的 `fluxnet_data/` 目录放入 `remixdb/data/`。

解压后应得到：

`remixdb/data/fluxnet_data/`

其中直接包含94个站点目录。每个站点目录保留该案例数据包中可用的 `HH`、`DD`、`WW`、`MM`、`YY` 分辨率的数据文件。

压缩包包含：

- 各站点的完整干净预处理文件 `*_pre.csv`。
- 根据项目故障生成配置制作的故障注入版本。

其中，FLUXNET 故障数据按照项目配置中的 DD 和 HH 分辨率生成。故障版本的文件名包含相应故障参数。

**FLUXNET 预处理方法**

本项目使用 `remixdb/tools/preprocess_fluxnet.py` 对原始 FLUXNET CSV 文件进行预处理，具体步骤如下：

1. 保留原始 `TIMESTAMP` 时间戳。
2. 按时间稳定排序。
3. 删除重复时间戳，同一时间戳保留原始文件中最先出现的记录。
4. 将空值、`NA`、`NaN`、无法解析的数值以及 FLUXNET 缺失标记 `-9999` 视为缺失值。
5. 对每个变量，最前面的连续缺失值使用该变量有效值的均值填补；之后遇到的缺失值使用前一个有效值进行前向填充。
6. 使用总体标准差进行 Z-score 标准化。
7. 将标准化结果截断至 `[-5, 5]`。

生成的 `*_pre.csv` 即为预处理后的干净数据。故障注入版本以这些预处理文件为输入生成。

**数据集引用：**

Pastorello, G., Trotta, C., Canfora, E., et al. (2020). The FLUXNET2015 dataset and the ONEFlux processing pipeline for eddy covariance data. *Scientific Data*, 7, 225. [https://doi.org/10.1038/s41597-020-0534-3](https://doi.org/10.1038/s41597-020-0534-3)。

使用 SPACETIME 案例数据时也请引用 [SPACETIME (AAAI 2025)](https://doi.org/10.1609/aaai.v39i18.34136)，并按 FLUXNET 政策列明所用站点 ID。

### 1.2 WindTunnel — `wt_walks_v1`

**数据来源与许可证**

- **原始数据集：** [Causal Chamber `wt_walks_v1`](https://github.com/juangamella/causal-chamber/tree/main/datasets/wt_walks_v1)。
- **随机游走生成脚本：** [actuators_random_walk.py](https://github.com/juangamella/causal-chamber/blob/main/datasets/wt_walks_v1/generators/actuators_random_walk.py)，其中生成1～16组随机游走。
- **原始压缩包：** [下载 `wt_walks_v1.zip`](https://causalchamber.s3.eu-central-1.amazonaws.com/downloadables/wt_walks_v1.zip)。
- **官方公布的原始压缩包 MD5：** `19bb4e92cbe0b8dff49299b9b509ac36`。
- **许可证：** [Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/)。上游数据仓库说明其 CSV 文件采用 CC BY 4.0，允许分享和改编，但需要署名并说明修改。
- **下载入口：** [GitHub Releases]。

**下载与解压**

从 [GitHub Releases] 下载 `windtunnel_data.zip`，解压到 `remixdb/data/`。

解压后应得到：

`remixdb/data/windtunnel_data/`

所有 CSV 文件应直接位于该目录中，不要额外嵌套一层 `windtunnel_data/`。

压缩包包含完整的31个干净预处理 CSV 文件：

- 16个 `actuators_random_walk_i_pre.csv`（i=1…16）及其合并文件 `actuators_random_walk_pre.csv`。
- 5个 `loads_hatch_mix_fast_run_i_pre.csv`（i=1…5）及其合并文件 `loads_hatch_mix_fast_run_pre.csv`。
- 5个 `loads_hatch_mix_slow_run_i_pre.csv`（i=1…5）及其合并文件 `loads_hatch_mix_slow_run_pre.csv`。
- `regime_jumps_single_pre.csv`。
- `regime_jumps_multi_pre.csv`。

三个合并文件分别由对应的分段预处理文件按数字顺序拼接而成，表头只保留一次。

此外，压缩包还包含上述预处理数据对应的故障注入版本，涵盖项目故障生成配置中的3%、5%、10%、15%、30%和50%故障比例。使用者无需重新生成这些故障文件。

**WindTunnel 预处理方法**

本项目对原始 WindTunnel 数据使用 `remixdb/tools/preprocess_windtunnel.py` 进行预处理。

具体步骤如下：

1. 保留 `timestamp` 列，不对时间戳进行标准化。
2. 对21个测量变量进行 Z-score 标准化，包括 `hatch`、`pot_1`、`pot_2`、`load_in`、`load_out`、`v_1`、`v_2`、`v_mic`、`v_in`、`v_out`、`current_in`、`current_out`、`rpm_in`、`rpm_out`、`pressure_upwind`、`pressure_downwind`、`pressure_ambient`、`pressure_intake`、`mic`、`signal_1` 和 `signal_2`。
3. 对无法解析为数值的内容以及缺失数值按0处理。
4. 标准化时使用样本标准差（分母为样本数减1）；对于标准差过小的变量，使用1作为标准化分母。
5. 对分段文件分别进行预处理，再按数字顺序拼接相应文件，合并文件的表头只保留一次。

生成的 `*_pre.csv` 即为预处理后的干净数据。故障注入版本以这些预处理文件为输入生成。

**数据集引用：**

Gamella, J. L., Peters, J., & Bühlmann, P. (2025). Causal chambers as a real-world physical testbed for AI methodology. *Nature Machine Intelligence*, 7, 107–118. [https://doi.org/10.1038/s42256-024-00964-x](https://doi.org/10.1038/s42256-024-00964-x)。

### 1.3 不随项目分发的数据集

以下数据集不通过 GitHub 源码仓库或 GitHub Releases 分发。本项目不提供其原始文件、预处理文件、故障文件、真值文件或样例片段。

使用者需要通过原始数据提供方或其他合法途径自行取得数据，并遵守相应使用条款。

| 数据集 | 来源与获取入口 | 不分发的原因 |
|---|---|---|
| **Basin** | [Global Runoff Data Centre (GRDC)](https://grdc.bafg.de/data/data_portal/index.html)；亦用于 [SPACETIME](https://doi.org/10.1609/aaai.v39i18.34136)。 | GRDC 对下载的水文数据设置了再分发限制，未经授权不得向公众或第三方（包括通过互联网）重新分发。因此项目不提供 Basin 数据及其衍生文件。 |
| **NETSIM** | [Oxford FMRIB NETSIM](https://www.fmrib.ox.ac.uk/datasets/netsim/index.html)；相关文件也见 [TCDF 的 fMRI 数据目录](https://github.com/M-Nauta/TCDF/tree/master/data/fMRI)。 | 未找到能够明确授权第三方公开再分发该 fMRI 数据的独立许可。TCDF 软件的许可证不自动授予其目录中第三方数据的再分发权。 |
| **DREAM3** | [DREAM Challenge](http://wiki.c2b2.columbia.edu/dream)；[GeneNetWeaver DREAM Challenge 数据归档](https://gnw.sourceforge.net/dreamchallenge.html)。 | 未找到能够明确授权公开再分发 DREAM3 表达数据及真值网络的许可。研究用途的数据获取不等同于公开再分发授权。 |

## 2. 使用公开数据集运行实验

完成第1节中的 FLUXNET 和 WindTunnel 数据集下载及解压后，即可直接编译并运行项目。

### 2.1 数据目录结构

两个数据库工程共用 `remixdb/data/` 数据目录。

正确的目录结构如下：

```text
InDB-LSS-CDiT/
├── remixdb/
│   ├── data/
│   │   ├── fluxnet_data/
│   │   │   ├── FLX_.../
│   │   │   ├── FLX_.../
│   │   │   └── ...
│   │   └── windtunnel_data/
│   │       ├── actuators_random_walk_1_pre.csv
│   │       ├── actuators_random_walk_1_pre_3%_...csv
│   │       ├── actuators_random_walk_2_pre.csv
│   │       ├── ...
│   │       ├── regime_jumps_single_pre.csv
│   │       └── regime_jumps_multi_pre.csv
│   ├── experiment/
│   ├── experiment_log/
│   └── xdbdemo.c
│
└── rocksdb/
    ├── experiment/
    ├── experiment_log/
    └── rdbdemo.c
```

其中：

- FLUXNET 数据按照站点目录组织，每个站点目录内包含对应的预处理文件和故障注入文件。
- WindTunnel 数据文件直接放在 `windtunnel_data/` 目录中。
- RocksDB 不需要另外复制一份数据集，而是从 `../remixdb/data/` 读取共享数据。

### 2.2 默认实验内容

默认情况下，RemixDB 和 RocksDB 均运行 FLUXNET（不含 ZA-Kru）与 WindTunnel 的实验。

对应的实验入口已经在源代码中启用，无需修改 `xdbdemo.c` 或 `rdbdemo.c`。

**RemixDB — `remixdb/xdbdemo.c`**

默认启用：

- `run_fluxnet_experiments()`：FLUXNET 数据库内实验。
- `run_outdb_fluxnet_experiments()`：FLUXNET 数据库外对照实验。
- `run_windtunnel_experiments()`：WindTunnel 数据库内实验。
- `run_outdb_windtunnel_experiments()`：WindTunnel 数据库外对照实验。

**RocksDB — `rocksdb/rdbdemo.c`**

默认启用：

- `run_fluxnet_experiments()`：FLUXNET 数据库内实验。
- `run_outdb_fluxnet_experiments()`：FLUXNET 数据库外对照实验。
- `run_windtunnel_experiments()`：WindTunnel 数据库内实验。
- `run_outdb_windtunnel_experiments()`：WindTunnel 数据库外对照实验。

### 2.3 选择是否运行故障注入实验

FLUXNET 和 WindTunnel 的公开压缩包已经包含故障注入版本，因此不需要另外运行 `realdataset_fault_generation`。

是否使用故障注入数据，由实验输入 CSV 中的 `是否测试故障数据集` 字段控制。

默认情况下，该字段为 `否`，使用干净的 `*_pre.csv` 文件运行实验。

如果需要运行故障注入实验，将对应实验输入 CSV 中的 `是否测试故障数据集` 修改为 `是`。

相关文件：

**RemixDB：**

- `remixdb/experiment/fluxnet_experiment_input.csv`
- `remixdb/experiment/windtunnel_experiment_input.csv`

**RocksDB：**

- `rocksdb/experiment/fluxnet_experiment_input.csv`
- `rocksdb/experiment/windtunnel_experiment_input.csv`

设置为 `是` 后，程序会自动识别对应目录中的故障注入文件，先运行干净数据，再运行可用的故障注入版本。

因此，使用者无需手动生成故障文件，也无需修改 `xdbdemo.c` 或 `rdbdemo.c` 中的实验入口。

### 2.4 RemixDB 编译与运行

在 Ubuntu 中打开终端，进入 `remixdb/`：

```bash
cd remixdb
make CCC=gcc XCC=g++ xdbdemo.out
./xdbdemo.out
```

在 CLion 中，也可以打开 `remixdb/` 工程，将 `remixdb/` 设置为工作目录，然后直接构建并运行 `xdbdemo.out`。

### 2.5 RocksDB 11.8.1 编译与运行

在 Ubuntu 中打开终端，进入 `rocksdb/`：

```bash
cd rocksdb
make -f Makefile.causal -j2 rdbdemo
./rdbdemo
```

在 VS Code 中，也可以打开 `rocksdb/` 作为工作区，选择 **Run and Debug → RocksDB experiment** 运行。

`Makefile.causal` 会先构建 RocksDB 静态库，再构建实验可执行文件。

RocksDB 从 `../remixdb/data/` 读取共享数据集。

### 2.6 Ubuntu 编译依赖

```bash
sudo apt update
sudo apt install build-essential gdb cmake gcc-11 g++-11 \
  libgflags-dev libsnappy-dev zlib1g-dev libbz2-dev \
  liblz4-dev libzstd-dev python3
```

实验日志分别写入 RemixDB 和 RocksDB 的 `experiment_log/` 目录。运行时数据库文件、元数据和日志会在运行过程中生成。

## 3. 自行取得不公开的数据集并运行实验

Basin、NETSIM 和 DREAM3 因数据授权问题不随项目分发。需要复现涉及这些数据集的实验时，使用者需要自行通过原始数据提供方或其他合法途径取得数据。

本节介绍各数据集所需的原始文件、目录结构及本项目实际采用的预处理方法。

### 3.1 Basin

**数据来源**

- 原始数据来源：[Global Runoff Data Centre (GRDC)](https://grdc.bafg.de/data/data_portal/index.html)。
- 案例研究来源：[SPACETIME: Causal Discovery from Non-Stationary Time Series](https://doi.org/10.1609/aaai.v39i18.34136)。

Basin 数据来自 GRDC 的欧洲358个流域时间序列。

本项目使用的主要变量为：

- `time`：时间。
- `tavg`：平均气温。
- `prec`：降水量。
- `Qobs`：实测径流量。

**原始数据准备**

1. 通过 GRDC 等合法途径取得原始流域数据。
2. 每个流域整理为一个 CSV 文件，放入 `remixdb/data/basin_data/`。
3. 每个 CSV 必须包含以下四列，且列顺序必须一致：

   `time,tavg,prec,Qobs`

   如果原始下载文件包含其他列，需要先整理出上述四列。

**Basin 预处理方法**

本项目使用 `remixdb/tools/preprocess_basin.py` 进行预处理。

具体步骤如下：

1. 将日期统一整理为 `YYYY/M/D` 格式。
2. 对 `tavg` 的缺失值进行线性插值；首尾缺失段分别使用最靠近的有效值填补。
3. 将 `prec` 的缺失值填充为0。
4. 对 `Qobs`：
   - 开头的缺失段填充为0。
   - 中间的缺失段使用线性插值。
   - 末尾的缺失段以前一个有效值向前填充。
5. 对非负的 `prec` 和 `Qobs` 执行 `log1p` 变换；负值保持不变。
6. 输出预处理后的 CSV 文件，文件名为 `<原文件名>_pre.csv`，与原始文件放在同一目录。

在 `remixdb/` 目录执行：

```bash
python3 tools/preprocess_basin.py data/basin_data
```

完成后即可得到 Basin 的预处理文件。

### 3.2 NETSIM

**数据来源**

- 数据集介绍：[Oxford FMRIB NETSIM](https://www.fmrib.ox.ac.uk/datasets/netsim/index.html)。
- 数据获取：[TCDF 的 fMRI 数据目录](https://github.com/M-Nauta/TCDF/tree/master/data/fMRI)。
- 相关论文：Efficient Constraint-based Window Causal Graph Discovery in Time Series with Multiple Time Lags。

**原始数据准备**

1. 将 NETSIM 原始时间序列放入：

   `remixdb/data/netsim/`

2. 文件名按程序约定为：

   `sim3_subject_0.csv` 至 `sim3_subject_49.csv`。

3. 将对应的真值邻接矩阵放在同一目录，命名为：

   `sim3_subject_0_truth.csv` 至 `sim3_subject_49_truth.csv`。

**NETSIM 预处理方法**

NETSIM 的预处理已经实现于：

`remixdb/experiment/cdit_experiment.cpp`

不需要单独执行 Python 预处理脚本。

调用 `run_cdit_experiments()` 时，程序会检查预处理文件是否存在。如果不存在，则读取原始时间序列，对每一行执行 Z-score 标准化，并生成：

`sim3_subject_N_pre.csv`

其中 N 为0～49。

具体而言，每一行数据分别计算该行各变量的均值和标准差，再进行标准化。标准差过小时，该行对应的标准化结果置为0。

预处理后的数据用于后续 CDiT 实验。

**实验配置**

NETSIM 实验配置文件已经包含在项目中：

`remixdb/experiment/netism_cdit_experiment_input.csv`

可以根据实际需要调整试验品集合、最大滞后和显著性水平。

### 3.3 DREAM3

**数据来源**

- 挑战赛来源：[DREAM Challenge](http://wiki.c2b2.columbia.edu/dream)。
- 数据归档：[GeneNetWeaver DREAM Challenge](https://gnw.sourceforge.net/dreamchallenge.html)。
- 原始挑战赛论文：Prill et al. (2010). Towards a Rigorous Assessment of Systems Biology Models: The DREAM3 Challenges. [https://doi.org/10.1371/journal.pone.0009202](https://doi.org/10.1371/journal.pone.0009202)。

**原始数据准备**

DREAM3 需要准备表达矩阵和对应的真值调控网络。

1. 将原始表达矩阵放入：

   `remixdb/data/dream3/Dream3TensorData/`

   程序使用的文件名包括：

   - `Size10Ecoli1.csv`
   - `Size10Ecoli2.csv`
   - `Size10Yeast1.csv`
   - `Size10Yeast2.csv`
   - `Size10Yeast3.csv`

   以及对应的 Size50 和 Size100 文件。

2. 将真值调控网络放入：

   `remixdb/data/dream3/TrueGeneNetworks/`

   文件名按程序约定，例如：

   - `InSilicoSize10-Ecoli1.txt`
   - `InSilicoSize10-Ecoli2.txt`
   - `InSilicoSize10-Yeast1.txt`
   - `InSilicoSize10-Yeast2.txt`
   - `InSilicoSize10-Yeast3.txt`

   同时需要包含相应的 Size50 和 Size100 文件。

**DREAM3 预处理方法**

DREAM3 的预处理已经实现于：

`remixdb/experiment/cdit_experiment.cpp`

不需要单独执行 Python 预处理脚本。

调用 `run_cdit_experiments()` 时，程序会检查预处理文件是否存在。如果不存在，则读取原始表达矩阵，对每一列执行 Z-score 标准化，并生成对应的 `_pre.csv` 文件。

具体而言，每一列分别计算其在所有时间点上的均值和标准差，再进行标准化。标准差过小时，该列对应的标准化结果置为0。

预处理后的表达矩阵用于后续 CDiT 实验。

**实验配置**

DREAM3 实验配置文件已经包含在项目中：

`remixdb/experiment/dream3_cdit_experiment_input.csv`

可以根据实际需要调整实验对象、最大滞后和显著性水平。

### 3.4 启用 Basin、NETSIM 和 DREAM3 实验

默认情况下，Basin、NETSIM 和 DREAM3 的实验入口均已注释，不会在默认运行时执行。

完成相应数据集的原始数据准备和预处理后，可以按照下面的说明启用实验。

**RemixDB — `remixdb/xdbdemo.c`**

打开 `remixdb/xdbdemo.c`，找到 `run_multiple_experiments()`。

其中 NETSIM 和 DREAM3 的入口为：

```cpp
/* NETSIM 与 DREAM3：取得原始数据并完成准备后，可启用此入口。 */
// rc = run_cdit_experiments();
// if (rc != 0) return rc;
```

准备好 NETSIM 和 DREAM3 两个数据集后，取消上述两行的注释：

```cpp
rc = run_cdit_experiments();
if (rc != 0) return rc;
```

注意：`run_cdit_experiments()` 会依次执行 NETSIM 和 DREAM3 两个数据集的实验。因此启用该入口前，需要同时准备好两个数据集。

Basin 的入口为：

```cpp
/* Basin：取得原始数据并完成预处理后，可启用相应实验。 */
// rc = run_basin_experiments();
// if (rc != 0) return rc;
// rc = run_outdb_basin_experiments();
// if (rc != 0) return rc;
```

准备好 Basin 数据并完成预处理后，取消上述四行的注释，即可启用 Basin 的数据库内和数据库外实验。

**RocksDB — `rocksdb/rdbdemo.c`**

RocksDB 支持 Basin 实验，不包含 NETSIM/DREAM3 基线实验入口。

打开 `rocksdb/rdbdemo.c`，找到 `run_multiple_experiments()`。

Basin 的入口为：

```cpp
/* Basin：取得原始数据并完成预处理后，可启用相应实验。 */
// rc = run_basin_experiments();
// if (rc != 0) return rc;
// rc = run_outdb_basin_experiments();
// if (rc != 0) return rc;
```

准备好 Basin 数据并完成预处理后，取消上述四行的注释，即可启用 Basin 实验。

完成修改后，重新编译并运行对应数据库工程即可。

## 4. 生成真实数据集的故障片段

对于已经通过 GitHub Releases 提供的 FLUXNET 和 WindTunnel 数据集，故障注入版本已经包含在压缩包中，**无需再次运行故障生成程序**。

RemixDB 的 `realdataset_fault_generation` 工具仍然保留，支持 Basin、FLUXNET 和 WindTunnel。它以干净的 `*_pre.csv` 为输入，在本地生成故障片段和延迟修复片段。

对于自行取得的 Basin 数据，如果需要生成故障注入版本，可以使用该工具。

操作步骤如下：

1. 确保已经取得 Basin 原始数据，并按照第3.1节完成预处理。
2. 打开 `remixdb/experiment/realdataset_fault_generation_input.csv`，根据需要调整故障比例、故障长度和修复延迟范围。
3. 打开 `remixdb/xdbdemo.c`，在 `main()` 中注释默认的：

   ```cpp
   return run_multiple_experiments();
   ```

   取消以下入口的注释：

   ```cpp
   return run_realdataset_fault_generation_experiments();
   ```

4. 在 `remixdb/` 目录编译并运行：

   ```bash
   make CCC=gcc XCC=g++ xdbdemo.out
   ./xdbdemo.out
   ```

5. 故障生成完成后，恢复 `main()` 中的默认入口并重新编译。若要在 Basin 实验中使用生成的故障文件，将 Basin 实验输入 CSV 中的 `是否测试故障数据集` 设置为 `是`。

**注意：** 每次运行故障生成工具时，程序会先清理 `basin_data/`、`fluxnet_data/` 和 `windtunnel_data/` 下此前生成的故障版本，再根据当前配置重新生成；干净的 `*_pre.csv` 源文件会保留。

因此，如果已经下载了 FLUXNET 和 WindTunnel 的公开故障版本，在运行该工具之前应注意备份，或者在运行后重新下载对应的公开数据集，以恢复原有故障文件。

该工具属于 RemixDB，RocksDB 没有单独的故障生成入口。

## 5. 软件许可证与第三方数据集声明

RemixDB 和 RocksDB 保留各自上游许可证，二者许可证相互独立。

第三方数据集的来源、许可证、署名和修改说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

---

# InDB-LSS-CDiT (English)

This repository contains two independently buildable database projects: RemixDB and RocksDB 11.8.1. Both projects use the shared data directory `remixdb/data/`. The GitHub source repository does not bundle dataset files or demo samples. The complete preprocessed FLUXNET and WindTunnel datasets, including clean data and fault-injected variants, are distributed separately through [GitHub Releases].

## 1. Dataset Downloads and Availability

This project publicly distributes the following two datasets through GitHub Releases:

- FLUXNET2015: 94 eligible sites from the SPACETIME case-study package, excluding ZA-Kru.
- WindTunnel: the complete Causal Chamber `wt_walks_v1` dataset.

Both archives contain clean preprocessed data and their corresponding fault-injected variants. Users can download and extract the archives and run the experiments without separately generating fault-injected data.

**File naming convention**

The `_pre.csv` suffix indicates a clean, preprocessed data file. For example:

`actuators_random_walk_1_pre.csv`

Fault-injected files are generated from the corresponding preprocessed files. Their filenames append fault parameters after `_pre`, for example:

`actuators_random_walk_1_pre_3%_[20,1500]_[50,2500].csv`

These parameters describe the fault ratio, fault-segment length range, and repair-delay range.

### 1.1 FLUXNET2015 — SPACETIME Case-Study Data (Excluding ZA-Kru)

**Data sources and license**

- Case-study paper: Mameche, S., Cornanguer, L., Ninad, U., & Vreeken, J. (2025). [SPACETIME: Causal Discovery from Non-Stationary Time Series](https://doi.org/10.1609/aaai.v39i18.34136).
- Case-study data package shared by the SPACETIME authors: [Google Drive archive](https://drive.google.com/file/d/1u8FJah1JolRf8pp_rnAB8u4NTNk8MYMJ/view?usp=sharing).
- Original dataset: [FLUXNET2015](https://fluxnet.org/data/fluxnet2015-dataset/).
- Policy: [FLUXNET2015 Data Policy](https://fluxnet.org/data/data-policy/) and [Data Change Log](https://fluxnet.org/change-log/).
- License for the included Tier One sites: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
- Download: [GitHub Releases].

The SPACETIME case-study package lists 95 FLUXNET site directories. `ZA-Kru` is a Tier Two site and is excluded. The remaining 94 sites fall under the CC BY 4.0 (Tier One) policy.

FLUXNET lists six Tier Two sites: `RU-Sam`, `RU-SkP`, `RU-Tks`, `RU-Vrk`, `SE-St1`, and `ZA-Kru`; only `ZA-Kru` occurs in the 95-site SPACETIME package.

This release covers the 94 eligible sites in the SPACETIME case-study package, not all global FLUXNET2015 sites. The FLUXNET policy applies at the site-year level; other FLUXNET copies or releases must be checked against the applicable policy for each site-year.

**Download and extraction**

Download all five FLUXNET archive files from [GitHub Releases]:

- `fluxnet_data.z01`
- `fluxnet_data.z02`
- `fluxnet_data.z03`
- `fluxnet_data.z04`
- `fluxnet_data.zip`

Place all five files in the same directory. Extract `fluxnet_data.zip` using WinRAR or 7-Zip, and place the extracted `fluxnet_data/` directory in `remixdb/data/`.

The resulting directory must be:

`remixdb/data/fluxnet_data/`

It contains the 94 site directories. Each site directory retains all available resolutions (`HH`, `DD`, `WW`, `MM`, and `YY`) from the case-study package.

The archive contains:

- Complete clean preprocessed files `*_pre.csv` for the included sites.
- Fault-injected variants generated using the project's fault-generation configurations.

The FLUXNET fault-injected files cover the DD and HH resolutions configured by the project. Their filenames include the corresponding fault parameters.

**FLUXNET preprocessing**

This project preprocesses the original FLUXNET CSV files using `remixdb/tools/preprocess_fluxnet.py`.

The procedure is as follows:

1. Preserve the original `TIMESTAMP` values.
2. Stably sort records by timestamp.
3. Remove duplicate timestamps, retaining the first occurrence in the original file.
4. Treat blank values, `NA`, `NaN`, unparseable values, and the FLUXNET missing-value marker `-9999` as missing.
5. For each variable, fill initial consecutive missing values with the mean of its valid values. Fill subsequent missing values using forward filling from the preceding valid value.
6. Apply Z-score normalization using the population standard deviation.
7. Clip normalized values to `[-5, 5]`.

The resulting `*_pre.csv` files are the clean preprocessed data. Fault-injected variants are generated from these files.

**Dataset citations:**

Pastorello, G., Trotta, C., Canfora, E., et al. (2020). "The FLUXNET2015 dataset and the ONEFlux processing pipeline for eddy covariance data." *Scientific Data*, 7, 225. https://doi.org/10.1038/s41597-020-0534-3.

Please also cite [SPACETIME (AAAI 2025)](https://doi.org/10.1609/aaai.v39i18.34136) when using the case-study data, and list the site IDs used as required by the FLUXNET policy.

### 1.2 WindTunnel — `wt_walks_v1`

**Data sources and license**

- Original dataset: [Causal Chamber `wt_walks_v1`](https://github.com/juangamella/causal-chamber/tree/main/datasets/wt_walks_v1).
- Random-walk generator: [actuators_random_walk.py](https://github.com/juangamella/causal-chamber/blob/main/datasets/wt_walks_v1/generators/actuators_random_walk.py), which generates 16 random-walk runs.
- Original archive: [Download `wt_walks_v1.zip`](https://causalchamber.s3.eu-central-1.amazonaws.com/downloadables/wt_walks_v1.zip).
- Published original archive MD5: `19bb4e92cbe0b8dff49299b9b509ac36`.
- License: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), as stated by the upstream repository for its CSV files.
- Download: [GitHub Releases].

**Download and extraction**

Download `windtunnel_data.zip` from [GitHub Releases] and extract it into `remixdb/data/`.

The resulting directory must be:

`remixdb/data/windtunnel_data/`

All CSV files must be directly inside this directory. Do not create an additional nested `windtunnel_data/` directory.

The archive contains all 31 clean preprocessed CSV files:

- 16 `actuators_random_walk_i_pre.csv` files (i=1…16) and their concatenated file `actuators_random_walk_pre.csv`.
- 5 `loads_hatch_mix_fast_run_i_pre.csv` files (i=1…5) and their concatenated file `loads_hatch_mix_fast_run_pre.csv`.
- 5 `loads_hatch_mix_slow_run_i_pre.csv` files (i=1…5) and their concatenated file `loads_hatch_mix_slow_run_pre.csv`.
- `regime_jumps_single_pre.csv`.
- `regime_jumps_multi_pre.csv`.

The three concatenated files are generated by combining their corresponding segment files in numeric order, retaining the header only once.

In addition, the archive contains fault-injected variants corresponding to these preprocessed files, covering the project's configured fault ratios of 3%, 5%, 10%, 15%, 30%, and 50%. Users do not need to generate these files separately.

**WindTunnel preprocessing**

This project preprocesses the original WindTunnel data using `remixdb/tools/preprocess_windtunnel.py`.

The procedure is as follows:

1. Preserve the `timestamp` column without standardizing it.
2. Apply Z-score normalization to the 21 measured variables: `hatch`, `pot_1`, `pot_2`, `load_in`, `load_out`, `v_1`, `v_2`, `v_mic`, `v_in`, `v_out`, `current_in`, `current_out`, `rpm_in`, `rpm_out`, `pressure_upwind`, `pressure_downwind`, `pressure_ambient`, `pressure_intake`, `mic`, `signal_1`, and `signal_2`.
3. Treat missing values and values that cannot be parsed as numeric as zero.
4. Use the sample standard deviation (denominator n−1) for normalization. For variables with an extremely small standard deviation, use 1 as the normalization denominator.
5. Preprocess individual segment files separately, then concatenate the corresponding files in numeric order, retaining the header only once.

The resulting `*_pre.csv` files are the clean preprocessed data. Fault-injected variants are generated from these files.

**Citation:**

Gamella, J. L., Peters, J., & Bühlmann, P. (2025). "Causal chambers as a real-world physical testbed for AI methodology." *Nature Machine Intelligence*, 7, 107–118. https://doi.org/10.1038/s42256-024-00964-x.

### 1.3 Datasets Not Distributed with This Project

The following datasets are not distributed through the GitHub source repository or GitHub Releases. This project does not provide their raw files, preprocessed files, fault-injected files, ground-truth files, or sample files.

Users must obtain these datasets independently through their original providers or other lawful sources and comply with the applicable terms of use.

| Dataset | Source and access | Reason for non-distribution |
|---|---|---|
| **Basin** | [Global Runoff Data Centre (GRDC)](https://grdc.bafg.de/data/data_portal/index.html); also used in [SPACETIME](https://doi.org/10.1609/aaai.v39i18.34136). | GRDC restricts redistribution of downloaded hydrological data to unauthorized third parties or the public, including online distribution. Therefore, this project does not provide Basin data or derived files. |
| **NETSIM** | [Oxford FMRIB NETSIM](https://www.fmrib.ox.ac.uk/datasets/netsim/index.html); related files are available in the [TCDF fMRI data directory](https://github.com/M-Nauta/TCDF/tree/master/data/fMRI). | No independent dataset-specific permission for public redistribution of the third-party fMRI data has been identified. The TCDF software license does not automatically grant redistribution rights to third-party data. |
| **DREAM3** | [DREAM Challenge](http://wiki.c2b2.columbia.edu/dream); [GeneNetWeaver DREAM Challenge archive](https://gnw.sourceforge.net/dreamchallenge.html). | No clear public redistribution permission has been identified for the DREAM3 expression data or ground-truth networks. Research access does not imply permission for public redistribution. |

## 2. Running Experiments with the Public Datasets

After downloading and extracting the FLUXNET and WindTunnel datasets described in Section 1, users can directly build and run the projects.

### 2.1 Data Directory Structure

Both database projects use the shared `remixdb/data/` directory.

The directory structure should be:

```text
InDB-LSS-CDiT/
├── remixdb/
│   ├── data/
│   │   ├── fluxnet_data/
│   │   │   ├── FLX_.../
│   │   │   ├── FLX_.../
│   │   │   └── ...
│   │   └── windtunnel_data/
│   │       ├── actuators_random_walk_1_pre.csv
│   │       ├── actuators_random_walk_1_pre_3%_...csv
│   │       ├── actuators_random_walk_2_pre.csv
│   │       ├── ...
│   │       ├── regime_jumps_single_pre.csv
│   │       └── regime_jumps_multi_pre.csv
│   ├── experiment/
│   ├── experiment_log/
│   └── xdbdemo.c
│
└── rocksdb/
    ├── experiment/
    ├── experiment_log/
    └── rdbdemo.c
```

- FLUXNET data are organized by site. Each site directory contains its preprocessed files and fault-injected variants.
- WindTunnel CSV files are placed directly in `windtunnel_data/`.
- RocksDB does not require a second copy of the datasets. It reads the shared data through `../remixdb/data/`.

### 2.2 Default Experiments

By default, both RemixDB and RocksDB run experiments on FLUXNET (excluding ZA-Kru) and WindTunnel.

The corresponding experiment entry points are already enabled in the source code. No changes to `xdbdemo.c` or `rdbdemo.c` are required.

**RemixDB — `remixdb/xdbdemo.c`**

Enabled by default:

- `run_fluxnet_experiments()`: FLUXNET in-database experiments.
- `run_outdb_fluxnet_experiments()`: FLUXNET out-of-database baseline experiments.
- `run_windtunnel_experiments()`: WindTunnel in-database experiments.
- `run_outdb_windtunnel_experiments()`: WindTunnel out-of-database baseline experiments.

**RocksDB — `rocksdb/rdbdemo.c`**

Enabled by default:

- `run_fluxnet_experiments()`: FLUXNET in-database experiments.
- `run_outdb_fluxnet_experiments()`: FLUXNET out-of-database baseline experiments.
- `run_windtunnel_experiments()`: WindTunnel in-database experiments.
- `run_outdb_windtunnel_experiments()`: WindTunnel out-of-database baseline experiments.

### 2.3 Selecting Clean or Fault-Injected Experiments

The public FLUXNET and WindTunnel archives already contain fault-injected variants. There is no need to run `realdataset_fault_generation` again.

Whether fault-injected data are used is controlled by the `是否测试故障数据集` field in the experiment input CSV files.

By default, this field is set to `否`, so experiments use clean `*_pre.csv` files.

To run fault-injected experiments, change this field to `是` in the corresponding experiment input CSV files.

**RemixDB:**

- `remixdb/experiment/fluxnet_experiment_input.csv`
- `remixdb/experiment/windtunnel_experiment_input.csv`

**RocksDB:**

- `rocksdb/experiment/fluxnet_experiment_input.csv`
- `rocksdb/experiment/windtunnel_experiment_input.csv`

When set to `是`, the program automatically detects available fault-injected files, runs the clean data first, and then runs the matching fault-injected variants.

Therefore, users do not need to generate fault-injected files manually or modify the experiment entry points in `xdbdemo.c` or `rdbdemo.c`.

### 2.4 Building and Running RemixDB

In Ubuntu, open a terminal and enter the `remixdb/` directory:

```bash
cd remixdb
make CCC=gcc XCC=g++ xdbdemo.out
./xdbdemo.out
```

Alternatively, open `remixdb/` in CLion, set `remixdb/` as the working directory, and build and run `xdbdemo.out`.

### 2.5 Building and Running RocksDB 11.8.1

In Ubuntu, open a terminal and enter the `rocksdb/` directory:

```bash
cd rocksdb
make -f Makefile.causal -j2 rdbdemo
./rdbdemo
```

Alternatively, open `rocksdb/` as a VS Code workspace and select **Run and Debug → RocksDB experiment**.

`Makefile.causal` builds the RocksDB static library first and then builds the experiment executable.

RocksDB reads the shared datasets from `../remixdb/data/`.

### 2.6 Ubuntu Build Dependencies

```bash
sudo apt update
sudo apt install build-essential gdb cmake gcc-11 g++-11 \
  libgflags-dev libsnappy-dev zlib1g-dev libbz2-dev \
  liblz4-dev libzstd-dev python3
```

Experiment logs are written to the respective `experiment_log/` directories. Runtime database files, metadata, and logs are generated during execution.

## 3. Obtaining and Preparing Non-Distributed Datasets

Basin, NETSIM, and DREAM3 are not distributed with this project because of their data-use and redistribution restrictions.

Users wishing to reproduce experiments involving these datasets must obtain them independently through their original providers or other lawful sources.

This section describes the required raw files, directory structures, and the preprocessing methods actually used by this project.

### 3.1 Basin

**Data source**

- Original data source: [Global Runoff Data Centre (GRDC)](https://grdc.bafg.de/data/data_portal/index.html).
- Case-study source: [SPACETIME: Causal Discovery from Non-Stationary Time Series](https://doi.org/10.1609/aaai.v39i18.34136).

The Basin dataset originates from time series collected from 358 European catchments in the GRDC database.

The main variables used by this project are:

- `time`: timestamp.
- `tavg`: average temperature.
- `prec`: precipitation.
- `Qobs`: observed river discharge.

**Preparing the raw data**

1. Obtain the original catchment data through GRDC or another lawful access route.
2. Organize each catchment as one CSV file under:

   `remixdb/data/basin_data/`

3. Each CSV must contain the following four columns in this exact order:

   `time,tavg,prec,Qobs`

   If the downloaded files contain additional columns, first arrange the data into this four-column format.

**Basin preprocessing**

This project uses `remixdb/tools/preprocess_basin.py` for preprocessing.

The procedure is as follows:

1. Normalize dates to the `YYYY/M/D` format.
2. Linearly interpolate missing `tavg` values. Leading and trailing missing segments are filled using the nearest valid value.
3. Fill missing `prec` values with zero.
4. For `Qobs`:
   - Fill the leading missing segment with zero.
   - Linearly interpolate internal missing segments.
   - Forward-fill trailing missing segments using the last valid value.
5. Apply `log1p` to non-negative `prec` and `Qobs` values. Negative values remain unchanged.
6. Write the preprocessed CSV beside the original file using the filename `<original-name>_pre.csv`.

Run the following command from `remixdb/`:

```bash
python3 tools/preprocess_basin.py data/basin_data
```

The preprocessed Basin files are then ready for the experiments.

### 3.2 NETSIM

**Data sources**

- Dataset information: [Oxford FMRIB NETSIM](https://www.fmrib.ox.ac.uk/datasets/netsim/index.html).
- Dataset files: [TCDF fMRI data directory](https://github.com/M-Nauta/TCDF/tree/master/data/fMRI).
- Related paper: Efficient Constraint-based Window Causal Graph Discovery in Time Series with Multiple Time Lags.

**Preparing the raw data**

1. Place the original NETSIM time-series files in:

   `remixdb/data/netsim/`

2. The expected filenames are:

   `sim3_subject_0.csv` through `sim3_subject_49.csv`.

3. Place the corresponding ground-truth adjacency matrices in the same directory, named:

   `sim3_subject_0_truth.csv` through `sim3_subject_49_truth.csv`.

**NETSIM preprocessing**

NETSIM preprocessing is implemented in:

`remixdb/experiment/cdit_experiment.cpp`

No separate Python preprocessing script is required.

When `run_cdit_experiments()` is called, the program checks whether the preprocessed files already exist. If they do not, it reads the raw time series, applies row-wise Z-score normalization, and generates:

`sim3_subject_N_pre.csv`

where N ranges from 0 to 49.

Specifically, each row is normalized independently using the mean and standard deviation of the variables in that row. If the standard deviation is too small, the normalized values for that row are set to zero.

The resulting preprocessed data are used in the subsequent CDiT experiments.

**Experiment configuration**

The NETSIM experiment configuration is included in:

`remixdb/experiment/netism_cdit_experiment_input.csv`

Users can adjust the subject sets, maximum lag, and significance level as needed.

### 3.3 DREAM3

**Data sources**

- Challenge source: [DREAM Challenge](http://wiki.c2b2.columbia.edu/dream).
- Data archive: [GeneNetWeaver DREAM Challenge](https://gnw.sourceforge.net/dreamchallenge.html).
- Original challenge paper: Prill et al. (2010). Towards a Rigorous Assessment of Systems Biology Models: The DREAM3 Challenges. https://doi.org/10.1371/journal.pone.0009202.

**Preparing the raw data**

DREAM3 requires both expression matrices and their corresponding ground-truth regulatory networks.

1. Place the original expression matrices in:

   `remixdb/data/dream3/Dream3TensorData/`

   The expected filenames include:

   - `Size10Ecoli1.csv`
   - `Size10Ecoli2.csv`
   - `Size10Yeast1.csv`
   - `Size10Yeast2.csv`
   - `Size10Yeast3.csv`

   Include the corresponding Size50 and Size100 files as well.

2. Place the ground-truth regulatory networks in:

   `remixdb/data/dream3/TrueGeneNetworks/`

   Expected filenames include:

   - `InSilicoSize10-Ecoli1.txt`
   - `InSilicoSize10-Ecoli2.txt`
   - `InSilicoSize10-Yeast1.txt`
   - `InSilicoSize10-Yeast2.txt`
   - `InSilicoSize10-Yeast3.txt`

   Include the corresponding Size50 and Size100 files as well.

**DREAM3 preprocessing**

DREAM3 preprocessing is implemented in:

`remixdb/experiment/cdit_experiment.cpp`

No separate Python preprocessing script is required.

When `run_cdit_experiments()` is called, the program checks whether the preprocessed files already exist. If they do not, it reads the original expression matrices, applies column-wise Z-score normalization, and generates the corresponding `_pre.csv` files.

Specifically, each column is normalized independently using its mean and standard deviation across all time points. If the standard deviation is too small, the normalized values for that column are set to zero.

The resulting preprocessed expression matrices are used in the subsequent CDiT experiments.

**Experiment configuration**

The DREAM3 experiment configuration is included in:

`remixdb/experiment/dream3_cdit_experiment_input.csv`

Users can adjust the experiment subjects, maximum lag, and significance level as needed.

### 3.4 Enabling Basin, NETSIM, and DREAM3 Experiments

By default, the experiment entry points for Basin, NETSIM, and DREAM3 are commented out. They will not be executed during the default run.

After obtaining the corresponding raw datasets and completing preprocessing, enable the relevant experiment entry points as described below.

**RemixDB — `remixdb/xdbdemo.c`**

Open `remixdb/xdbdemo.c` and locate `run_multiple_experiments()`.

The NETSIM and DREAM3 entry point is:

```cpp
/* NETSIM and DREAM3: enable after obtaining and preparing the raw datasets. */
// rc = run_cdit_experiments();
// if (rc != 0) return rc;
```

After preparing both NETSIM and DREAM3, uncomment these two lines:

```cpp
rc = run_cdit_experiments();
if (rc != 0) return rc;
```

Note that `run_cdit_experiments()` executes both NETSIM and DREAM3 sequentially. Both datasets must therefore be prepared before enabling this entry point.

The Basin entry points are:

```cpp
/* Basin: enable after obtaining and preprocessing the raw data. */
// rc = run_basin_experiments();
// if (rc != 0) return rc;
// rc = run_outdb_basin_experiments();
// if (rc != 0) return rc;
```

After preparing and preprocessing Basin, uncomment all four lines to enable both the in-database and out-of-database Basin experiments.

**RocksDB — `rocksdb/rdbdemo.c`**

RocksDB supports Basin experiments but does not include the NETSIM/DREAM3 baseline entry point.

Open `rocksdb/rdbdemo.c` and locate `run_multiple_experiments()`.

The Basin entry points are:

```cpp
/* Basin: enable after obtaining and preprocessing the raw data. */
// rc = run_basin_experiments();
// if (rc != 0) return rc;
// rc = run_outdb_basin_experiments();
// if (rc != 0) return rc;
```

After preparing and preprocessing Basin, uncomment all four lines to enable the Basin experiments.

Rebuild and run the corresponding database project after making these changes.

## 4. Generating Faulted Real-Dataset Files

The public FLUXNET and WindTunnel archives already include their fault-injected variants. **Users do not need to run the fault-generation utility for these two datasets.**

The RemixDB `realdataset_fault_generation` utility is still available and supports Basin, FLUXNET, and WindTunnel. It generates faulted and delayed-repair variants locally from clean `*_pre.csv` files.

For Basin, which is not distributed with the project, users can use this utility if fault-injected data are required.

The procedure is as follows:

1. Obtain the original Basin data and complete preprocessing as described in Section 3.1.
2. Open `remixdb/experiment/realdataset_fault_generation_input.csv` and adjust the fault ratio, fault-segment length, and repair-delay ranges as needed.
3. Open `remixdb/xdbdemo.c`. In `main()`, comment out the default entry point:

   ```cpp
   return run_multiple_experiments();
   ```

   Uncomment the following entry point:

   ```cpp
   return run_realdataset_fault_generation_experiments();
   ```

4. Build and run from `remixdb/`:

   ```bash
   make CCC=gcc XCC=g++ xdbdemo.out
   ./xdbdemo.out
   ```

5. After fault generation finishes, restore the default `main()` entry point and rebuild. To use the generated Basin fault files in experiments, set `是否测试故障数据集` to `是` in the Basin experiment input CSV.

**Important:** Each fault-generation run first removes previously generated fault variants under `basin_data/`, `fluxnet_data/`, and `windtunnel_data/`, then regenerates them according to the current configuration. The clean `*_pre.csv` source files are retained.

Therefore, if you have already downloaded the public FLUXNET and WindTunnel fault-injected variants, back them up before running this utility, or download the corresponding public archives again afterward to restore the original fault files.

Fault generation is provided by RemixDB. RocksDB has no separate fault-generation entry point.

## 5. Software Licenses and Third-Party Dataset Notices

RemixDB and RocksDB retain their respective upstream licenses, which are independent of each other.

For dataset sources, licenses, attribution requirements, and modification notices, see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
