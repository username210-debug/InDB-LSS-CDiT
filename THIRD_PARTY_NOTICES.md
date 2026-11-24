
# 第三方许可证与数据集声明

本文档说明本项目使用的第三方软件、数据集的来源、许可证、署名要求、预处理修改及公开分发范围。

## 1. 第三方软件

本项目包含两套上游数据库工程，两者独立构建，分别遵循各自的上游许可证。

- **RemixDB**（`remixdb/`）：保留上游项目的 `LICENSE` 文件。使用及分发时应遵守该源码目录中的许可证条款。
- **RocksDB 11.8.1**（`rocksdb/`）：保留上游的 `LICENSE`、`COPYING` 以及第三方依赖许可证文件。使用及分发时应遵守 RocksDB 适用的许可证及其所包含第三方依赖的许可证。
- 本项目新增或修改的源代码文件，应按照相应上游许可证的适用要求进行使用和分发。本声明不替代、不合并，也不改变各上游项目原有的许可证。

## 2. 第三方数据集声明

### 2.1 FLUXNET2015——SPACETIME 案例数据中的94个站点（排除 ZA-Kru）

**数据来源与许可证**

- 原始数据集：[FLUXNET2015](https://fluxnet.org/data/fluxnet2015-dataset/)。
- 官方数据政策：[FLUXNET Data Policy](https://fluxnet.org/data/data-policy/)。
- 官方数据政策变更记录：[FLUXNET Change Log](https://fluxnet.org/change-log/)。
- 本项目使用的 Tier One 站点数据适用 [Creative Commons Attribution 4.0 International（CC BY 4.0）](https://creativecommons.org/licenses/by/4.0/)。
- SPACETIME 案例数据包：[Google Drive 数据包](https://drive.google.com/file/d/1u8FJah1JolRf8pp_rnAB8u4NTNk8MYMJ/view?usp=sharing)。
- 公开下载：[GitHub Releases — Experimental Datasets v1.0](https://github.com/username210-debug/InDB-LSS-CDiT/releases/tag/datasets-v1.0)。

SPACETIME 案例数据包列出了95个 FLUXNET 站点目录。根据 FLUXNET2015 官方数据政策，以下六个站点属于 Tier Two：

`RU-Sam`、`RU-SkP`、`RU-Tks`、`RU-Vrk`、`SE-St1`、`ZA-Kru`。

在上述95个案例站点中，只有 `ZA-Kru` 与 Tier Two 名单重合。因此，本项目已排除 `ZA-Kru`，仅公开其余94个符合 Tier One 数据政策的站点。

本项目公开的是 SPACETIME 案例数据包中的94个合规站点，并非 FLUXNET2015 全球全部站点。FLUXNET 数据政策按站点年份（site-year）适用；使用其他来源、版本或年份的数据时，仍应根据官方政策核对对应站点年份。

**公开文件及预处理说明**

GitHub Release 提供剩余94个站点的完整预处理文件，包括案例数据包中可用的以下时间分辨率：

- `HH`
- `DD`
- `WW`
- `MM`
- `YY`

FLUXNET 压缩包由以下五个文件组成：

- `fluxnet_data.z01`
- `fluxnet_data.z02`
- `fluxnet_data.z03`
- `fluxnet_data.z04`
- `fluxnet_data.zip`

必须将五个文件放在同一目录中，然后使用 WinRAR 或 7-Zip 解压 `fluxnet_data.zip`。

公开压缩包仅包含干净的预处理 `*_pre.csv` 文件，不包含原始 CSV 文件或故障注入版本。

预处理过程包括：

1. 保留时间戳。
2. 按时间稳定排序。
3. 删除重复时间戳，同一时间戳保留原始文件中最先出现的记录。
4. 将空值、`NA`、`NaN` 和 FLUXNET 缺失标记 `-9999` 视为缺失值。
5. 每列最前面的缺失值使用该列有效值的均值填补；之后的缺失值使用前一个有效值进行前向填充。
6. 使用总体标准差进行 Z-score 标准化。
7. 将标准化结果截断至 `[-5, 5]`。

以上文件属于经过修改和预处理的第三方数据集。数据来源及预处理修改情况已在项目 README 和本声明中说明。

使用 FLUXNET 数据进行研究时，应引用原始数据集论文，列明实际使用的 FLUXNET 站点 ID，并根据适用政策在必要时引用各站点的独立 DOI。

**原始数据集论文：**

Pastorello, G., Trotta, C., Canfora, E., et al. (2020). "The FLUXNET2015 dataset and the ONEFlux processing pipeline for eddy covariance data." *Scientific Data*, 7, 225.

DOI: <https://doi.org/10.1038/s41597-020-0534-3>。

### 2.2 Causal Chamber WindTunnel——`wt_walks_v1`

**数据来源与许可证**

- 原始数据集：[Causal Chamber `wt_walks_v1`](https://github.com/juangamella/causal-chamber/tree/main/datasets/wt_walks_v1)。
- 随机游走生成脚本：[actuators_random_walk.py](https://github.com/juangamella/causal-chamber/blob/main/datasets/wt_walks_v1/generators/actuators_random_walk.py)。
- 原始压缩包：[wt_walks_v1.zip](https://causalchamber.s3.eu-central-1.amazonaws.com/downloadables/wt_walks_v1.zip)。
- 上游公布的原始压缩包 MD5：`19bb4e92cbe0b8dff49299b9b509ac36`。
- 数据集许可证：[Creative Commons Attribution 4.0 International（CC BY 4.0）](https://creativecommons.org/licenses/by/4.0/)。上游 Causal Chamber 仓库说明其 CSV 数据文件采用 CC BY 4.0，允许分享和改编，但需要署名并说明修改。
- 公开下载：[GitHub Releases — Experimental Datasets v1.0](https://github.com/username210-debug/InDB-LSS-CDiT/releases/tag/datasets-v1.0)。

**公开文件及预处理说明**

本项目公开与所使用的上游 `wt_walks_v1` 压缩包版本相对应的完整预处理 CSV 文件集。

公开压缩包为 `windtunnel_data.zip`，包含全部31个干净预处理 CSV 文件：

- 16个独立的 actuator random-walk 文件，以及对应的合并文件 `actuators_random_walk_pre.csv`。
- 5个独立的 fast-load 文件，以及对应的合并文件 `loads_hatch_mix_fast_run_pre.csv`。
- 5个独立的 slow-load 文件，以及对应的合并文件 `loads_hatch_mix_slow_run_pre.csv`。
- 2个状态突变文件：`regime_jumps_single_pre.csv` 和 `regime_jumps_multi_pre.csv`。

预处理过程保留 `timestamp` 列，对21个测量变量进行 Z-score 标准化，并将无法解析为数值的内容按0处理。

三个合并文件分别按照对应分段文件的数字顺序拼接，表头只保留一次。

公开压缩包仅包含干净的预处理 `*_pre.csv` 文件，不包含原始 CSV 文件或故障注入版本。

以上文件属于经过修改和预处理的上游数据。使用、发表研究结果或进一步分发时，应按照 CC BY 4.0 要求进行署名，并说明所作的修改。

**原始数据集论文：**

Gamella, J. L., Peters, J., & Bühlmann, P. (2025). "Causal chambers as a real-world physical testbed for AI methodology." *Nature Machine Intelligence*, 7, 107–118.

DOI: <https://doi.org/10.1038/s42256-024-00964-x>。

### 2.3 FLUXNET 案例数据包的额外来源说明

本项目使用的 FLUXNET 站点数据来源于 SPACETIME 论文作者分享的案例数据包：

<https://drive.google.com/file/d/1u8FJah1JolRf8pp_rnAB8u4NTNk8MYMJ/view?usp=sharing>

原始案例数据包包含95个 FLUXNET 站点目录。本项目排除了 Tier Two 站点 `ZA-Kru`，因此公开数据中保留94个符合条件的站点。

以下论文用于说明本项目所选 FLUXNET 案例数据包的来源及研究背景。需要特别说明的是：该论文是案例数据包的来源说明，**不是 FLUXNET2015 原始数据集的介绍论文**。

Mameche, S., Cornanguer, L., Ninad, U., & Vreeken, J. (2025). "SPACETIME: Causal Discovery from Non-Stationary Time Series." *Proceedings of the AAAI Conference on Artificial Intelligence*, 39(18), 19405–19413.

DOI: <https://doi.org/10.1609/aaai.v39i18.34136>。

### 2.4 Basin——不公开分发

- 案例研究来源：SPACETIME，<https://doi.org/10.1609/aaai.v39i18.34136>。
- 原始数据来源：Global Runoff Data Centre（GRDC），<https://grdc.bafg.de/data/data_portal/index.html>。
- GRDC 对下载的水文数据设置了再分发限制，未经授权不得向公众或第三方重新分发，包括通过互联网公开分发。仅进行分析或进一步处理，并不意味着自动获得公开分发原始数据及其衍生文件的许可。
- 本项目不包含 Basin 原始数据、预处理数据、故障注入数据、真值文件或样例片段。

### 2.5 NETSIM——不公开分发

- 数据集信息：[Oxford FMRIB NETSIM](https://www.fmrib.ox.ac.uk/datasets/netsim/index.html)。
- 相关数据文件：[TCDF 的 fMRI 数据目录](https://github.com/M-Nauta/TCDF/tree/master/data/fMRI)。
- 尚未找到能够明确授权第三方公开再分发该 fMRI 数据的独立许可。TCDF 软件的许可证并不自动授予其目录中第三方数据的再分发权。
- 本项目不包含 NETSIM 原始数据、预处理数据、真值文件或样例片段。

### 2.6 DREAM3——不公开分发

- 挑战赛来源：[DREAM Challenge](http://wiki.c2b2.columbia.edu/dream)。
- 数据归档：[GeneNetWeaver DREAM Challenge](https://gnw.sourceforge.net/dreamchallenge.html)。
- 原始挑战赛论文：Prill et al., "Towards a Rigorous Assessment of Systems Biology Models: The DREAM3 Challenges" (2010)，DOI：<https://doi.org/10.1371/journal.pone.0009202>。
- 尚未找到能够明确授权公开再分发 DREAM3 表达数据及真值网络的许可。研究用途的数据获取或作者提供的数据访问权限，并不等同于公开再分发授权。
- 本项目不包含 DREAM3 原始数据、预处理数据、真值文件或样例片段。

## 3. 数据集公开分发范围与使用说明

本项目仅通过 GitHub Releases 公开分发以下两个数据集：

- FLUXNET：94个符合条件的站点的完整预处理数据。
- WindTunnel：`wt_walks_v1` 的完整预处理数据。

公开下载入口：

<https://github.com/username210-debug/InDB-LSS-CDiT/releases/tag/datasets-v1.0>

两个数据集的公开压缩包均只包含干净的预处理 `*_pre.csv` 文件，不包含故障注入数据或延迟修复数据。

部分实验所需的故障注入数据可以使用 RemixDB 提供的 `realdataset_fault_generation` 工具在本地生成。具体操作请参阅项目 README。

Basin、NETSIM 和 DREAM3 不通过本项目的 GitHub 源码仓库或 GitHub Releases 分发。需要复现涉及这些数据集的实验时，使用者必须通过原始数据提供方或其他合法途径自行取得，并遵守相应的数据使用政策。

第三方数据集的权利与软件许可证相互独立。本项目软件的许可证不授予任何第三方数据集的再分发权。

---

# Third-Party Notices

This document describes the third-party software and datasets used in this project, including their sources, licenses, attribution requirements, preprocessing modifications, and public redistribution scope.

## 1. Third-Party Software

This repository contains two upstream database projects. They are independently buildable and retain their separate upstream licenses.

- **RemixDB** (`remixdb/`): the upstream `LICENSE` file is retained. Use and redistribution must comply with the license terms in that source tree.
- **RocksDB 11.8.1** (`rocksdb/`): the upstream `LICENSE`, `COPYING`, and third-party dependency license files are retained. Use and redistribution must comply with the applicable RocksDB license and the licenses of its bundled dependencies.
- Project-added and project-modified source files must be used and redistributed consistently with the applicable upstream license obligations. This notice does not replace, combine, or modify the original licenses of the upstream projects.

## 2. Third-Party Dataset Notices

### 2.1 FLUXNET2015 — 94 Sites from the SPACETIME Case-Study Package (Excluding ZA-Kru)

**Data sources and license**

- Original dataset: [FLUXNET2015](https://fluxnet.org/data/fluxnet2015-dataset/).
- Official data policy: [FLUXNET Data Policy](https://fluxnet.org/data/data-policy/).
- Official policy change log: [FLUXNET Change Log](https://fluxnet.org/change-log/).
- The Tier One site data used in this project are covered by [Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/).
- SPACETIME case-study data package: [Google Drive archive](https://drive.google.com/file/d/1u8FJah1JolRf8pp_rnAB8u4NTNk8MYMJ/view?usp=sharing).
- Public download: [GitHub Releases — Experimental Datasets v1.0](https://github.com/username210-debug/InDB-LSS-CDiT/releases/tag/datasets-v1.0).

The SPACETIME case-study package lists 95 FLUXNET site directories. According to the official FLUXNET2015 data policy, the following six sites are classified as Tier Two:

`RU-Sam`, `RU-SkP`, `RU-Tks`, `RU-Vrk`, `SE-St1`, and `ZA-Kru`.

Among the 95 sites in the case-study package, only `ZA-Kru` overlaps with the Tier Two list. Therefore, this project excludes `ZA-Kru` and distributes only the remaining 94 sites covered by the Tier One data policy.

The distributed data represent the 94 eligible sites in the SPACETIME case-study package, not all sites in the global FLUXNET2015 dataset. The FLUXNET data policy applies at the site-year level. Users working with data from other sources, versions, or years must independently verify the applicable policy for each site-year.

**Distributed files and preprocessing**

The GitHub Release provides the complete preprocessed files for the remaining 94 sites, including all available temporal resolutions in the case-study package:

- `HH`
- `DD`
- `WW`
- `MM`
- `YY`

The FLUXNET archive consists of five files:

- `fluxnet_data.z01`
- `fluxnet_data.z02`
- `fluxnet_data.z03`
- `fluxnet_data.z04`
- `fluxnet_data.zip`

All five files must be placed in the same directory before extracting `fluxnet_data.zip` using WinRAR or 7-Zip.

The public archive contains clean preprocessed `*_pre.csv` files only. It does not contain raw CSV files or fault-injected variants.

The preprocessing procedure:

1. Preserves timestamps.
2. Stably sorts records by time.
3. Removes duplicate timestamps, retaining the first occurrence in the original file.
4. Treats blank values, `NA`, `NaN`, and the FLUXNET missing-value marker `-9999` as missing.
5. Fills initial missing values in each column with that column's mean of valid values, and forward-fills subsequent missing values using the preceding valid value.
6. Applies Z-score normalization using the population standard deviation.
7. Clips normalized values to `[-5, 5]`.

The distributed files are adapted and preprocessed versions of the third-party dataset. Their provenance and preprocessing modifications are documented in the project README and this notice.

When publishing research using FLUXNET data, users should cite the original dataset publication, list the FLUXNET site IDs used, and cite individual site DOIs when applicable under the relevant data policy.

**Original dataset publication:**

Pastorello, G., Trotta, C., Canfora, E., et al. (2020). "The FLUXNET2015 dataset and the ONEFlux processing pipeline for eddy covariance data." *Scientific Data*, 7, 225.

DOI: <https://doi.org/10.1038/s41597-020-0534-3>.

### 2.2 Causal Chamber WindTunnel — `wt_walks_v1`

**Data sources and license**

- Original dataset: [Causal Chamber `wt_walks_v1`](https://github.com/juangamella/causal-chamber/tree/main/datasets/wt_walks_v1).
- Random-walk generator: [actuators_random_walk.py](https://github.com/juangamella/causal-chamber/blob/main/datasets/wt_walks_v1/generators/actuators_random_walk.py).
- Original archive: [wt_walks_v1.zip](https://causalchamber.s3.eu-central-1.amazonaws.com/downloadables/wt_walks_v1.zip).
- Published upstream archive MD5: `19bb4e92cbe0b8dff49299b9b509ac36`.
- Dataset license: [Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/). The upstream Causal Chamber repository states that its CSV files are licensed under CC BY 4.0, permitting sharing and adaptation subject to attribution and indication of modifications.
- Public download: [GitHub Releases — Experimental Datasets v1.0](https://github.com/username210-debug/InDB-LSS-CDiT/releases/tag/datasets-v1.0).

**Distributed files and preprocessing**

This project distributes the complete set of preprocessed CSV files corresponding to the upstream `wt_walks_v1` archive version used to create them.

The public archive, `windtunnel_data.zip`, contains all 31 clean preprocessed CSV files:

- 16 individual actuator random-walk files and their concatenated file, `actuators_random_walk_pre.csv`.
- 5 individual fast-load files and their concatenated file, `loads_hatch_mix_fast_run_pre.csv`.
- 5 individual slow-load files and their concatenated file, `loads_hatch_mix_slow_run_pre.csv`.
- 2 regime-jump files: `regime_jumps_single_pre.csv` and `regime_jumps_multi_pre.csv`.

The preprocessing preserves the `timestamp` column, applies Z-score normalization to the 21 measured variables, and treats values that cannot be parsed as numeric as zero.

The three concatenated files are generated by combining their corresponding segment files in numeric order, retaining the header only once.

The public archive contains clean preprocessed `*_pre.csv` files only. It does not contain raw CSV files or fault-injected variants.

These files are modified and preprocessed versions of the upstream dataset. Users must provide appropriate attribution and indicate the modifications when using, publishing results based on, or further redistributing these files, in accordance with CC BY 4.0.

**Original dataset publication:**

Gamella, J. L., Peters, J., & Bühlmann, P. (2025). "Causal chambers as a real-world physical testbed for AI methodology." *Nature Machine Intelligence*, 7, 107–118.

DOI: <https://doi.org/10.1038/s42256-024-00964-x>.

### 2.3 Additional Provenance for the FLUXNET Case-Study Subset

The FLUXNET site package used in this project originates from the case-study data package shared by the authors of SPACETIME:

<https://drive.google.com/file/d/1u8FJah1JolRf8pp_rnAB8u4NTNk8MYMJ/view?usp=sharing>

The original case-study package contains 95 FLUXNET site directories. This project excludes the Tier Two site `ZA-Kru`, leaving 94 eligible sites in the distributed dataset.

The following paper describes the case study and its use of FLUXNET data. It is cited to acknowledge the provenance of the selected FLUXNET case-study package. Importantly, this paper is **not the original publication describing the FLUXNET2015 dataset**.

Mameche, S., Cornanguer, L., Ninad, U., & Vreeken, J. (2025). "SPACETIME: Causal Discovery from Non-Stationary Time Series." *Proceedings of the AAAI Conference on Artificial Intelligence*, 39(18), 19405–19413.

DOI: <https://doi.org/10.1609/aaai.v39i18.34136>.

### 2.4 Basin — Not Distributed

- Case-study source: SPACETIME, <https://doi.org/10.1609/aaai.v39i18.34136>.
- Original data source: Global Runoff Data Centre (GRDC), <https://grdc.bafg.de/data/data_portal/index.html>.
- GRDC terms restrict redistribution of downloaded hydrological data to unauthorized third parties or the public, including online distribution. Analysis or further processing does not by itself grant permission to publicly redistribute the original data or derived files.
- No Basin raw data, preprocessed data, fault-injected data, ground-truth files, or sample files are included in this project.

### 2.5 NETSIM — Not Distributed

- Dataset information: [Oxford FMRIB NETSIM](https://www.fmrib.ox.ac.uk/datasets/netsim/index.html).
- Related data files: [TCDF fMRI data directory](https://github.com/M-Nauta/TCDF/tree/master/data/fMRI).
- No independent dataset-specific permission for public redistribution of the third-party fMRI data has been identified. The TCDF software license does not automatically grant redistribution rights to third-party data contained in its repository.
- No NETSIM raw data, preprocessed data, ground-truth files, or sample files are included in this project.

### 2.6 DREAM3 — Not Distributed

- Challenge source: [DREAM Challenge](http://wiki.c2b2.columbia.edu/dream).
- Data archive: [GeneNetWeaver DREAM Challenge](https://gnw.sourceforge.net/dreamchallenge.html).
- Original challenge paper: Prill et al., "Towards a Rigorous Assessment of Systems Biology Models: The DREAM3 Challenges" (2010). DOI: <https://doi.org/10.1371/journal.pone.0009202>.
- No clear public redistribution permission has been identified for the DREAM3 expression data and ground-truth networks. Research access to data, including access provided by an author, does not itself establish permission for public redistribution.
- No DREAM3 raw data, preprocessed data, ground-truth files, or sample files are included in this project.

## 3. Dataset Redistribution Scope and Usage

Only the following two datasets are publicly distributed through GitHub Releases:

- FLUXNET: complete preprocessed data for 94 eligible sites.
- WindTunnel: complete preprocessed data for `wt_walks_v1`.

Public download:

<https://github.com/username210-debug/InDB-LSS-CDiT/releases/tag/datasets-v1.0>

Both public archives contain clean preprocessed `*_pre.csv` files only. Fault-injected data and delayed-repair data are not included.

Fault-injected data required by certain experiments can be generated locally using the `realdataset_fault_generation` utility provided by RemixDB. See the project README for instructions.

Basin, NETSIM, and DREAM3 are not distributed through this project's GitHub source repository or GitHub Releases. Users wishing to reproduce experiments involving these datasets must obtain them independently through their original providers or other lawful sources and comply with the applicable data-use policies.

Dataset rights and software licenses are independent. The software license of this project does not grant rights to redistribute third-party datasets.
