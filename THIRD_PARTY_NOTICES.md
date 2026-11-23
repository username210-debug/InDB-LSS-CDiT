# Third-Party Notices

## Software

This repository contains two upstream database projects. Their licenses remain separate.

- **RemixDB** (`remixdb/`): the upstream `LICENSE` file is retained. Follow the license terms in that source tree.
- **RocksDB 11.8.1** (`rocksdb/`): the upstream `LICENSE`, `COPYING`, and third-party license files are retained. Follow the applicable RocksDB license and the licenses of bundled dependencies.
- Project-added and project-modified source files must be used consistently with the applicable upstream license obligations. This notice does not replace or combine the upstream licenses.

## Dataset notices

### FLUXNET2015 — 94 Tier One sites from the SPACETIME case-study package

- Case-study paper: Mameche, S., Cornanguer, L., Ninad, U., & Vreeken, J. (2025). “SPACETIME: Causal Discovery from Non-Stationary Time Series.” *Proceedings of the AAAI Conference on Artificial Intelligence*, 39(18), 19405–19413. DOI: <https://doi.org/10.1609/aaai.v39i18.34136>.
- Case-study data package: <https://drive.google.com/file/d/1u8FJah1JolRf8pp_rnAB8u4NTNk8MYMJ/view?usp=sharing>.
- Original dataset: FLUXNET2015, <https://fluxnet.org/data/fluxnet2015-dataset/>.
- Data policy: <https://fluxnet.org/data/data-policy/>; change log: <https://fluxnet.org/change-log/>.
- The official FLUXNET2015 policy permits sharing and adaptation of CC BY 4.0 (Tier One) data with attribution. The six Tier Two sites are RU-Sam, RU-SkP, RU-Tks, RU-Vrk, SE-St1, and ZA-Kru. Among the 95 site directories listed in the SPACETIME case-study package, ZA-Kru is the only Tier Two site; it is excluded from the distributed derivative dataset and from project preprocessing, fault generation, and experiment discovery.
- The Zenodo record distributes the complete preprocessed files for the remaining 94 Tier One sites from the case-study package. The preprocessed files preserve timestamps, sort records by time, remove duplicate timestamps, impute missing numeric values, apply population z-score normalization, and clip normalized values to [-5, 5]. This is an adapted dataset; attribution and a description of these preprocessing changes are provided in the project README and Zenodo record.
- Required dataset citation: Pastorello, G., Trotta, C., Canfora, E., et al. (2020). “The FLUXNET2015 dataset and the ONEFlux processing pipeline for eddy covariance data.” *Scientific Data*, 7, 225. DOI: <https://doi.org/10.1038/s41597-020-0534-3>.

### Causal Chamber WindTunnel `wt_walks_v1` — distributed separately through Zenodo

- Source: <https://github.com/juangamella/causal-chamber/tree/main/datasets/wt_walks_v1>.
- Original archive: <https://causalchamber.s3.eu-central-1.amazonaws.com/downloadables/wt_walks_v1.zip>.
- Published upstream archive MD5: `19bb4e92cbe0b8dff49299b9b509ac36`.
- Dataset license: CC BY 4.0. The upstream Causal Chamber repository states that its CSV files are licensed under CC BY 4.0: <https://creativecommons.org/licenses/by/4.0/>.
- Citation: Gamella, J. L., Peters, J., & Bühlmann, P. (2025). “Causal chambers as a real-world physical testbed for AI methodology.” *Nature Machine Intelligence*. DOI: <https://doi.org/10.1038/s42256-024-00964-x>.
- The Zenodo record distributes the complete preprocessed CSV set corresponding to the upstream `wt_walks_v1` archive version used to create it. The timestamp column is preserved and the 21 measured variables are z-score normalized. The derived files are modified/preprocessed versions of the upstream data; the Zenodo record and project README identify the changes.

### Basin — not distributed

- Case-study source: the SPACETIME study, <https://doi.org/10.1609/aaai.v39i18.34136>.
- Original data source: Global Runoff Data Centre (GRDC), <https://grdc.bafg.de/data/data_portal/index.html>.
- GRDC terms restrict redistribution of downloaded hydrological data to unauthorized persons, third parties, or the public, including online distribution. Analysis and further processing do not by themselves grant permission to publicly redistribute the source data or derived files.
- No Basin raw data, preprocessed data, faulted variants, ground-truth files, or samples are included.

### NETSIM — not distributed

- Dataset information: Oxford FMRIB NETSIM, <https://www.fmrib.ox.ac.uk/datasets/netsim/index.html>.
- Related data files: <https://github.com/M-Nauta/TCDF/tree/master/data/fMRI>.
- No dataset-specific public redistribution permission was identified for the third-party fMRI data. The TCDF software license does not by itself grant redistribution rights to those data.
- No NETSIM raw data, preprocessed data, ground-truth files, or samples are included.

### DREAM3 — not distributed

- Challenge source: <http://wiki.c2b2.columbia.edu/dream>.
- Data archive: <https://gnw.sourceforge.net/dreamchallenge.html>.
- Original challenge paper: Prill et al., “Towards a Rigorous Assessment of Systems Biology Models: The DREAM3 Challenges” (2010), DOI: <https://doi.org/10.1371/journal.pone.0009202>.
- No clear public redistribution permission was identified for the DREAM3 expression data and ground-truth networks. Access supplied by an author for research does not itself establish public redistribution permission.
- No DREAM3 raw data, preprocessed data, ground-truth files, or samples are included.

Dataset rights and software licenses are independent. The license of the software in this repository does not grant rights to redistribute third-party datasets.
