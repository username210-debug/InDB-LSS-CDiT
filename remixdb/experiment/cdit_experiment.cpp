/*
 * 本文件实现 NETSIM 与 DREAM3 数据集上的离线基线实验，负责读取实验配置、调用分析接口并记录评价指标。
 */

#include "cdit_experiment.h"

#include "../cdit_module.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

    struct ExperimentMetric {
        double f1 = 0.0;
        double shd = 0.0;
    };

    struct NetsimConfig {
        std::vector<std::string> subjects;
        int tau = 0;
        double alpha = 0.01;
        bool use_preprocessed = false;
    };

    struct Dream3Config {
        std::vector<std::string> subjects;
        int tau = 0;
        double alpha = 0.01;
        bool use_preprocessed = false;
    };

    static const char* kNetsimInputCsv = "experiment/netism_cdit_experiment_input.csv";
    static const char* kDream3InputCsv = "experiment/dream3_cdit_experiment_input.csv";

    static const char* kNetsimLogCsv = "experiment_log/对Netsim的库外纯CDiT实验.csv";
    static const char* kDream3LogCsv = "experiment_log/对Dream3的库外纯CDiT实验.csv";

    static const char* kNetsimDir = "data/netsim";
    static const char* kDream3TensorDir = "data/dream3/Dream3TensorData";
    static const char* kDream3TruthDir = "data/dream3/TrueGeneNetworks";

    static bool file_exists(const std::string& path) {
        return access(path.c_str(), F_OK) == 0;
    }

    static bool ensure_parent_dir_exists(const std::string& path) {
        std::string::size_type pos = path.find_last_of('/');
        if (pos == std::string::npos) return true;
        std::string dir = path.substr(0, pos);
        struct stat st;
        if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
        return false;
    }

    static std::string trim(const std::string& s) {
        std::size_t b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        std::size_t e = s.size();
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    }

    static std::vector<std::string> split_csv_line(const std::string& line) {
        std::vector<std::string> fields;
        std::string cur;
        bool in_quotes = false;

        for (std::size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '"') {
                if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    in_quotes = !in_quotes;
                }
            } else if (c == ',' && !in_quotes) {
                fields.push_back(trim(cur));
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        fields.push_back(trim(cur));

        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].size() >= 2 && fields[i].front() == '"' && fields[i].back() == '"') {
                fields[i] = fields[i].substr(1, fields[i].size() - 2);
            }
        }
        return fields;
    }

    static std::vector<std::string> split_list_field(const std::string& s) {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = trim(item);
            if (!item.empty()) out.push_back(item);
        }
        return out;
    }

    static std::string fmt2(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << x;
        return oss.str();
    }

    static void init_csv_with_bom(const std::string& path, const std::string& header_line) {
        if (!ensure_parent_dir_exists(path)) {
            std::cerr << "无法创建日志文件所在目录: " << path << std::endl;
            return;
        }
        std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        ofs.write(reinterpret_cast<const char*>(bom), 3);
        ofs << header_line << "\n";
    }

    static void append_csv_line(const std::string& path, const std::vector<std::string>& fields) {
        std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::app);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            std::string v = fields[i];
            bool need_quote =
                    (v.find(',') != std::string::npos) ||
                    (v.find('"') != std::string::npos) ||
                    (v.find('\n') != std::string::npos);

            if (need_quote) {
                std::string escaped;
                for (std::size_t k = 0; k < v.size(); ++k) {
                    if (v[k] == '"') escaped += "\"\"";
                    else escaped.push_back(v[k]);
                }
                ofs << '"' << escaped << '"';
            } else {
                ofs << v;
            }

            if (i + 1 < fields.size()) ofs << ',';
        }
        ofs << "\n";
    }

    // 读取纯数值 CSV；忽略空行和无法完整解析为有限数值的行。
    static std::vector<std::vector<double>> load_numeric_csv(const std::string& path) {
        std::ifstream ifs(path.c_str());
        std::vector<std::vector<double>> data;
        if (!ifs) {
            std::cerr << "无法打开文件: " << path << std::endl;
            return data;
        }

        std::string line;
        while (std::getline(ifs, line)) {
            line = trim(line);
            if (line.empty()) continue;

            std::vector<std::string> fields = split_csv_line(line);
            std::vector<double> row;
            row.reserve(fields.size());

            bool ok = true;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                const std::string& f = fields[i];
                char* endptr = nullptr;
                double v = std::strtod(f.c_str(), &endptr);
                if (endptr == f.c_str() || *endptr != '\0' || !std::isfinite(v)) {
                    ok = false;
                    break;
                }
                row.push_back(v);
            }

            if (ok && !row.empty()) data.push_back(row);
        }
        return data;
    }

    static std::vector<std::vector<double>> transpose_matrix(const std::vector<std::vector<double>>& a) {
        if (a.empty()) return std::vector<std::vector<double>>();
        std::size_t rows = a.size();
        std::size_t cols = a[0].size();
        std::vector<std::vector<double>> b(cols, std::vector<double>(rows, 0.0));
        for (std::size_t i = 0; i < rows; ++i) {
            for (std::size_t j = 0; j < cols; ++j) {
                b[j][i] = a[i][j];
            }
        }
        return b;
    }

    // 读取 NETSIM 的真值邻接矩阵，并将数值矩阵转换为二值有向图。
    static std::vector<std::vector<int>> load_netsim_truth(const std::string& path) {
        std::vector<std::vector<double>> raw = load_numeric_csv(path);
        std::vector<std::vector<int>> g(raw.size(),
                                        std::vector<int>(raw.empty() ? 0 : raw[0].size(), 0));
        for (std::size_t i = 0; i < raw.size(); ++i) {
            for (std::size_t j = 0; j < raw[i].size(); ++j) {
                g[i][j] = (raw[i][j] > 0.5 ? 1 : 0);
            }
        }
        return g;
    }

    // 解析 DREAM3 真值文件中的基因调控边，并构造指定规模的邻接矩阵。
    static std::vector<std::vector<int>> parse_dream3_truth(const std::string& path, int n) {
        std::ifstream ifs(path.c_str());
        std::vector<std::vector<int>> g(n, std::vector<int>(n, 0));
        if (!ifs) {
            std::cerr << "无法打开真值文件: " << path << std::endl;
            return g;
        }

        std::string all((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());

        std::size_t pos = 0;
        while (true) {
            std::size_t g1 = all.find('G', pos);
            if (g1 == std::string::npos) break;
            std::size_t p1 = g1 + 1;
            while (p1 < all.size() && std::isdigit(static_cast<unsigned char>(all[p1]))) ++p1;
            if (p1 == g1 + 1) {
                pos = g1 + 1;
                continue;
            }
            int src = std::atoi(all.substr(g1 + 1, p1 - g1 - 1).c_str());

            std::size_t g2 = all.find('G', p1);
            if (g2 == std::string::npos) break;
            std::size_t p2 = g2 + 1;
            while (p2 < all.size() && std::isdigit(static_cast<unsigned char>(all[p2]))) ++p2;
            if (p2 == g2 + 1) {
                pos = g2 + 1;
                continue;
            }
            int dst = std::atoi(all.substr(g2 + 1, p2 - g2 - 1).c_str());

            if (src >= 1 && src <= n && dst >= 1 && dst <= n) {
                g[src - 1][dst - 1] = 1;
            }

            pos = p2;
        }
        return g;
    }

    static std::vector<std::vector<int>> inferred_summary_graph(const CDiTResult& r, int n) {
        std::vector<std::vector<int>> g(n, std::vector<int>(n, 0));
        for (std::map<std::pair<int,int>, std::vector<int>>::const_iterator it = r.lags_by_edge.begin();
             it != r.lags_by_edge.end(); ++it) {
            int u = it->first.first;
            int v = it->first.second;
            if (u >= 1 && u <= n && v >= 1 && v <= n && !it->second.empty()) {
                g[u - 1][v - 1] = 1;
            }
        }
        return g;
    }

    static ExperimentMetric calc_f1_shd(const std::vector<std::vector<int>>& pred,
                                        const std::vector<std::vector<int>>& truth) {
        ExperimentMetric m;
        const int n = static_cast<int>(truth.size());

        long long tp = 0;
        long long fp = 0;
        long long fn = 0;

        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                bool p = (i < static_cast<int>(pred.size()) &&
                          j < static_cast<int>(pred[i].size()) &&
                          pred[i][j] != 0);
                bool t = (truth[i][j] != 0);

                if (p && t) ++tp;
                else if (p && !t) ++fp;
                else if (!p && t) ++fn;
            }
        }

        double precision = (tp + fp) > 0 ? static_cast<double>(tp) / static_cast<double>(tp + fp) : 0.0;
        double recall    = (tp + fn) > 0 ? static_cast<double>(tp) / static_cast<double>(tp + fn) : 0.0;
        m.f1 = (precision + recall) > 0.0
               ? 2.0 * precision * recall / (precision + recall)
               : 0.0;

        int shd = 0;

        for (int i = 0; i < n; ++i) {
            bool p = (i < static_cast<int>(pred.size()) &&
                      i < static_cast<int>(pred[i].size()) &&
                      pred[i][i] != 0);
            bool t = (truth[i][i] != 0);
            if (p != t) ++shd;
        }

        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                bool pij = pred[i][j] != 0;
                bool pji = pred[j][i] != 0;
                bool tij = truth[i][j] != 0;
                bool tji = truth[j][i] != 0;

                if (pij == tij && pji == tji) continue;

                if (pij && !pji && !tij && tji) {
                    ++shd;
                } else if (!pij && pji && tij && !tji) {
                    ++shd;
                } else {
                    if (pij != tij) ++shd;
                    if (pji != tji) ++shd;
                }
            }
        }

        m.shd = static_cast<double>(shd);
        return m;
    }

    static std::vector<std::string> make_names(const std::string& prefix, int n) {
        std::vector<std::string> names;
        names.reserve(n);
        for (int i = 1; i <= n; ++i) {
            std::ostringstream oss;
            oss << prefix << i;
            names.push_back(oss.str());
        }
        return names;
    }

    static std::string pretty_or_empty(const std::string& s) {
        return s.empty() ? "（空）" : s;
    }

    static std::string join_subjects(const std::vector<std::string>& subjects) {
        std::ostringstream oss;
        for (std::size_t i = 0; i < subjects.size(); ++i) {
            if (i) oss << ',';
            oss << subjects[i];
        }
        return oss.str();
    }

    static std::string netsim_data_path(const std::string& sid, bool use_preprocessed) {
        if (sid.size() < 2 || sid[0] != 's') return std::string();
        std::string idx = sid.substr(1);
        std::string path = std::string(kNetsimDir) + "/sim3_subject_" + idx;
        if (use_preprocessed) path += "_pre";
        path += ".csv";
        return path;
    }

    static std::string netsim_truth_path(const std::string& sid) {
        if (sid.size() < 2 || sid[0] != 's') return std::string();
        std::string idx = sid.substr(1);
        return std::string(kNetsimDir) + "/sim3_subject_" + idx + "_truth.csv";
    }

    static bool parse_dream3_subject(const std::string& sid,
                                     int& size_out,
                                     std::string& organism_out,
                                     int& replica_out) {
        if (sid.size() < 5 || sid[0] != 'S') return false;

        std::size_t pos = 1;
        std::size_t start_digits = pos;
        while (pos < sid.size() && std::isdigit(static_cast<unsigned char>(sid[pos]))) ++pos;
        if (pos == start_digits || pos >= sid.size()) return false;

        size_out = std::atoi(sid.substr(start_digits, pos - start_digits).c_str());

        char org = sid[pos++];
        if (org == 'E') organism_out = "Ecoli";
        else if (org == 'Y') organism_out = "Yeast";
        else return false;

        if (pos >= sid.size()) return false;

        replica_out = std::atoi(sid.substr(pos).c_str());
        return replica_out > 0;
    }

    static std::string dream3_tensor_path(const std::string& sid, bool use_preprocessed) {
        int size = 0;
        int replica = 0;
        std::string organism;
        if (!parse_dream3_subject(sid, size, organism, replica)) return std::string();

        std::ostringstream oss;
        oss << kDream3TensorDir << "/Size" << size << organism << replica;
        if (use_preprocessed) oss << "_pre";
        oss << ".csv";
        return oss.str();
    }

    static std::string dream3_raw_tensor_path(const std::string& sid) {
        return dream3_tensor_path(sid, false);
    }

    static std::string dream3_truth_path(const std::string& sid) {
        int size = 0;
        int replica = 0;
        std::string organism;
        if (!parse_dream3_subject(sid, size, organism, replica)) return std::string();

        std::ostringstream oss;
        oss << kDream3TruthDir << "/InSilicoSize" << size << "-" << organism << replica << ".txt";
        return oss.str();
    }

    static void write_numeric_csv(const std::string& path,
                                  const std::vector<std::vector<double>>& data) {
        std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
        ofs << std::setprecision(17);
        for (std::size_t i = 0; i < data.size(); ++i) {
            for (std::size_t j = 0; j < data[i].size(); ++j) {
                if (j) ofs << ',';
                ofs << data[i][j];
            }
            ofs << "\n";
        }
    }

    static std::vector<std::vector<double>> zscore_columns(
            const std::vector<std::vector<double>>& data) {
        if (data.empty()) return std::vector<std::vector<double>>();

        const int rows = static_cast<int>(data.size());
        const int cols = static_cast<int>(data[0].size());

        std::vector<double> mean(cols, 0.0);
        std::vector<double> stdev(cols, 0.0);

        for (int j = 0; j < cols; ++j) {
            for (int i = 0; i < rows; ++i) mean[j] += data[i][j];
            mean[j] /= std::max(1, rows);

            for (int i = 0; i < rows; ++i) {
                double diff = data[i][j] - mean[j];
                stdev[j] += diff * diff;
            }
            stdev[j] = std::sqrt(stdev[j] / std::max(1, rows));
        }

        std::vector<std::vector<double>> out = data;
        for (int i = 0; i < rows; ++i) {
            for (int j = 0; j < cols; ++j) {
                if (stdev[j] > 1e-12) out[i][j] = (out[i][j] - mean[j]) / stdev[j];
                else out[i][j] = 0.0;
            }
        }
        return out;
    }

    static std::vector<std::vector<double>> zscore_rows(
            const std::vector<std::vector<double>>& data) {
        if (data.empty()) return std::vector<std::vector<double>>();

        const int rows = static_cast<int>(data.size());
        const int cols = static_cast<int>(data[0].size());

        std::vector<std::vector<double>> out = data;

        for (int i = 0; i < rows; ++i) {
            double mean = 0.0;
            for (int j = 0; j < cols; ++j) mean += data[i][j];
            mean /= std::max(1, cols);

            double stdev = 0.0;
            for (int j = 0; j < cols; ++j) {
                double diff = data[i][j] - mean;
                stdev += diff * diff;
            }
            stdev = std::sqrt(stdev / std::max(1, cols));

            for (int j = 0; j < cols; ++j) {
                if (stdev > 1e-12) out[i][j] = (out[i][j] - mean) / stdev;
                else out[i][j] = 0.0;
            }
        }

        return out;
    }

    // 按实验要求生成 NETSIM 的预处理数据；通过输出文件存在性避免重复预处理。
    static void preprocess_netsim_once() {
        struct stat dir_stat;
        if (stat(kNetsimDir, &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) {
            return;
        }
        for (int idx = 0; idx < 50; ++idx) {
            std::ostringstream sid_oss;
            sid_oss << 's' << idx;
            std::string sid = sid_oss.str();

            std::string raw_path = netsim_data_path(sid, false);
            std::string pre_path = netsim_data_path(sid, true);

            if (file_exists(pre_path)) continue;

            std::vector<std::vector<double>> data = load_numeric_csv(raw_path);
            if (data.empty()) {
                std::cerr << "Netsim预处理失败，原始数据为空: " << raw_path << std::endl;
                continue;
            }

            std::vector<std::vector<double>> normalized = zscore_rows(data);
            write_numeric_csv(pre_path, normalized);
        }
    }

    // 按实验要求生成 DREAM3 的预处理数据；通过输出文件存在性避免重复预处理。
    static void preprocess_dream3_once() {
        struct stat tensor_stat;
        struct stat truth_stat;
        if (stat(kDream3TensorDir, &tensor_stat) != 0 || !S_ISDIR(tensor_stat.st_mode) ||
            stat(kDream3TruthDir, &truth_stat) != 0 || !S_ISDIR(truth_stat.st_mode)) {
            return;
        }
        const char* subjects[] = {
                "S10E1", "S10E2", "S10Y1", "S10Y2", "S10Y3",
                "S50E1", "S50E2", "S50Y1", "S50Y2", "S50Y3",
                "S100E1", "S100E2", "S100Y1", "S100Y2", "S100Y3"
        };

        for (std::size_t i = 0; i < sizeof(subjects) / sizeof(subjects[0]); ++i) {
            std::string raw_path = dream3_raw_tensor_path(subjects[i]);
            std::string pre_path = dream3_tensor_path(subjects[i], true);

            if (file_exists(pre_path)) continue;

            std::vector<std::vector<double>> data = load_numeric_csv(raw_path);
            if (data.empty()) {
                std::cerr << "Dream3预处理失败，原始数据为空: " << raw_path << std::endl;
                continue;
            }

            std::vector<std::vector<double>> normalized = zscore_columns(data);
            write_numeric_csv(pre_path, normalized);
        }
    }

    // 读取 NETSIM 实验配置，解析试验品集合、最大滞后、显著性水平和预处理选项。
    static std::vector<NetsimConfig> load_netsim_configs() {
        std::ifstream ifs(kNetsimInputCsv);
        std::vector<NetsimConfig> cfgs;
        if (!ifs) {
            std::cerr << "无法打开Netsim实验参数文件: " << kNetsimInputCsv << std::endl;
            return cfgs;
        }

        std::string line;
        bool first = true;
        while (std::getline(ifs, line)) {
            line = trim(line);
            if (line.empty()) continue;

            std::vector<std::string> f = split_csv_line(line);

            if (first) {
                first = false;
                if (!f.empty() && f[0].find("试验品") != std::string::npos) continue;
            }

            if (f.size() < 4) continue;

            NetsimConfig cfg;
            cfg.subjects = split_list_field(f[0]);
            cfg.tau = std::atoi(f[1].c_str());
            cfg.alpha = std::atof(f[2].c_str());
            cfg.use_preprocessed = (f[3] == "是");

            if (!cfg.subjects.empty() && cfg.tau > 0 && cfg.alpha > 0.0) {
                cfgs.push_back(cfg);
            }
        }
        return cfgs;
    }

    // 读取 DREAM3 实验配置，解析试验品集合、最大滞后、显著性水平和预处理选项。
    static std::vector<Dream3Config> load_dream3_configs() {
        std::ifstream ifs(kDream3InputCsv);
        std::vector<Dream3Config> cfgs;
        if (!ifs) {
            std::cerr << "无法打开Dream3实验参数文件: " << kDream3InputCsv << std::endl;
            return cfgs;
        }

        std::string line;
        bool first = true;
        while (std::getline(ifs, line)) {
            line = trim(line);
            if (line.empty()) continue;

            std::vector<std::string> f = split_csv_line(line);

            if (first) {
                first = false;
                if (!f.empty() && f[0].find("试验品") != std::string::npos) continue;
            }

            if (f.size() < 4) continue;

            Dream3Config cfg;
            cfg.subjects = split_list_field(f[0]);
            cfg.tau = std::atoi(f[1].c_str());
            cfg.alpha = std::atof(f[2].c_str());
            cfg.use_preprocessed = (f[3] == "是");

            if (!cfg.subjects.empty() && cfg.tau > 0 && cfg.alpha > 0.0) {
                cfgs.push_back(cfg);
            }
        }
        return cfgs;
    }

    // 运行 NETSIM 库外基线实验，计算推断结果并与真值比较，输出 F1 和 SHD。
    static int run_netsim_experiments() {
        preprocess_netsim_once();

        std::vector<NetsimConfig> cfgs = load_netsim_configs();

        init_csv_with_bom(
                kNetsimLogCsv,
                "试验品集合,推断时的τ,显著性水平,是否预处理,耗时(s),平均F1分数,平均SHD"
        );

        for (std::size_t exp_idx = 0; exp_idx < cfgs.size(); ++exp_idx) {
            const NetsimConfig& cfg = cfgs[exp_idx];

            std::cout << "Netsim第" << (exp_idx + 1) << "次实验\n";
            std::cout << "本此数据库外纯CDiT推断实验参数设置：试验品集合="
                      << join_subjects(cfg.subjects)
                      << "；τ=" << cfg.tau
                      << "，显著性水平=" << cfg.alpha
                      << "，是否预处理=" << (cfg.use_preprocessed ? "是" : "否")
                      << "\n";
            std::cout << "推断结果如下\n";

            double sum_f1 = 0.0;
            double sum_shd = 0.0;

            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

            for (std::size_t i = 0; i < cfg.subjects.size(); ++i) {
                const std::string& sid = cfg.subjects[i];

                std::vector<std::vector<double>> raw =
                        load_numeric_csv(netsim_data_path(sid, cfg.use_preprocessed));
                std::vector<std::vector<double>> data = transpose_matrix(raw);
                std::vector<std::vector<int>> truth = load_netsim_truth(netsim_truth_path(sid));

                CDiTResult r = run_cdit(data, cfg.tau, cfg.alpha, true, nullptr, nullptr);
                std::vector<std::vector<int>> pred =
                        inferred_summary_graph(r, static_cast<int>(truth.size()));

                ExperimentMetric m = calc_f1_shd(pred, truth);
                sum_f1 += m.f1;
                sum_shd += m.shd;

                std::vector<std::string> names = make_names("X", static_cast<int>(truth.size()));

                std::cout << "对" << sid << " | "
                          << pretty_or_empty(format_inferred_named(r, names))
                          << "| F1:" << fmt2(m.f1)
                          << "；SHD:" << fmt2(m.shd) << "\n";
            }

            std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(t1 - t0).count();

            double avg_f1 = cfg.subjects.empty() ? 0.0 : sum_f1 / static_cast<double>(cfg.subjects.size());
            double avg_shd = cfg.subjects.empty() ? 0.0 : sum_shd / static_cast<double>(cfg.subjects.size());

            std::cout << "小节：耗时:" << fmt2(elapsed) << "s，平均F1分数:"
                      << fmt2(avg_f1) << "，平均SHD:" << fmt2(avg_shd) << "\n\n";

            append_csv_line(
                    kNetsimLogCsv,
                    std::vector<std::string>{
                            join_subjects(cfg.subjects),
                            std::to_string(cfg.tau),
                            fmt2(cfg.alpha),
                            (cfg.use_preprocessed ? "是" : "否"),
                            fmt2(elapsed),
                            fmt2(avg_f1),
                            fmt2(avg_shd)
                    }
            );
        }

        return 0;
    }

    // 运行 DREAM3 库外基线实验，计算推断结果并与真值比较，输出 F1 和 SHD。
    static int run_dream3_experiments() {
        preprocess_dream3_once();

        std::vector<Dream3Config> cfgs = load_dream3_configs();

        init_csv_with_bom(
                kDream3LogCsv,
                "试验品集合,推断时的τ,显著性水平,是否预处理,耗时(s),平均F1分数,平均SHD"
        );

        for (std::size_t exp_idx = 0; exp_idx < cfgs.size(); ++exp_idx) {
            const Dream3Config& cfg = cfgs[exp_idx];

            std::cout << "Dream3第" << (exp_idx + 1) << "次实验\n";
            std::cout << "本此数据库外纯CDiT推断实验参数设置：试验品集合="
                      << join_subjects(cfg.subjects)
                      << "；τ=" << cfg.tau
                      << "，显著性水平=" << cfg.alpha
                      << "，是否预处理=" << (cfg.use_preprocessed ? "是" : "否")
                      << "\n";
            std::cout << "推断结果如下\n";

            double sum_f1 = 0.0;
            double sum_shd = 0.0;

            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

            for (std::size_t i = 0; i < cfg.subjects.size(); ++i) {
                const std::string& sid = cfg.subjects[i];

                std::vector<std::vector<double>> data =
                        load_numeric_csv(dream3_tensor_path(sid, cfg.use_preprocessed));
                int n = data.empty() ? 0 : static_cast<int>(data[0].size());

                std::vector<std::vector<int>> truth = parse_dream3_truth(dream3_truth_path(sid), n);

                CDiTResult r = run_cdit(data, cfg.tau, cfg.alpha, false, nullptr, nullptr);
                std::vector<std::vector<int>> pred = inferred_summary_graph(r, n);

                ExperimentMetric m = calc_f1_shd(pred, truth);
                sum_f1 += m.f1;
                sum_shd += m.shd;

                std::vector<std::string> names = make_names("G", n);

                std::cout << "对" << sid << " | "
                          << pretty_or_empty(format_inferred_named(r, names))
                          << "| F1:" << fmt2(m.f1)
                          << "；SHD:" << fmt2(m.shd) << "\n";
            }

            std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(t1 - t0).count();

            double avg_f1 = cfg.subjects.empty() ? 0.0 : sum_f1 / static_cast<double>(cfg.subjects.size());
            double avg_shd = cfg.subjects.empty() ? 0.0 : sum_shd / static_cast<double>(cfg.subjects.size());

            std::cout << "小节：耗时:" << fmt2(elapsed) << "s，平均F1分数:"
                      << fmt2(avg_f1) << "，平均SHD:" << fmt2(avg_shd) << "\n\n";

            append_csv_line(
                    kDream3LogCsv,
                    std::vector<std::string>{
                            join_subjects(cfg.subjects),
                            std::to_string(cfg.tau),
                            fmt2(cfg.alpha),
                            (cfg.use_preprocessed ? "是" : "否"),
                            fmt2(elapsed),
                            fmt2(avg_f1),
                            fmt2(avg_shd)
                    }
            );
        }

        return 0;
    }

} // 匿名命名空间结束

// NETSIM 与 DREAM3 实验统一入口，依次执行两类数据集的基线实验。
extern "C" int run_cdit_experiments(void) {
    int rc1 = run_netsim_experiments();
    int rc2 = run_dream3_experiments();
    return (rc1 != 0) ? rc1 : rc2;
}
