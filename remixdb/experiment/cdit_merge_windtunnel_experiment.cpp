/*
 * 本文件实现 WindTunnel 数据集上的相邻窗口图合并实验及其统计日志。
 */

#include "cdit_merge_windtunnel_experiment.h"

#include "../cdit_module.h"
#include "../merge_cg.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <ios>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// 打印开关：
// 1) 默认仅打印“合并前后效果对比”
// 2) 若想额外打印“合并前”，把 kPrintBefore 改为 true
// 3) 若想额外打印“合并后”，把 kPrintAfter 改为 true
    static const bool kPrintBefore = false;
    static const bool kPrintAfter = false;
    static const bool kPrintCompare = true;

    static const double kDefaultAlpha = 0.01;
    static const char* kInputCsvPath = "experiment/cdit_merge_windtunnel_experiment_input.csv";
    static const char* kDataDirPrimary = "data/windtunnel_data/";
    static const char* kDataDirFallback = "data/windtunnel_experiment/";
    static const char* kStatCsvPath = "experiment_log/对风洞实验的库外CDiT+Merge实验统计.csv";
    static const char* kPrintLogPath = "experiment_log/对风洞实验的库外CDiT+Merge实验打印日志.txt";

    struct VariableDef {
        std::string short_name;
        std::string csv_name;
    };

    struct QueryRange {
        bool all = true;
        double start = -std::numeric_limits<double>::infinity();
        double end = std::numeric_limits<double>::infinity();
        std::string display;
        std::string csv_cell;
    };

    struct ExperimentConfig {
        std::string raw_trial_set;
        std::string raw_query_range;
        std::string raw_source_cols;
        std::string raw_target_cols;
        int sample_step = 1;
        int T = 0;
        int tau = 0;
        int Delta = 0;
        double theta = 0.0;

        std::vector<std::string> trial_aliases;
        QueryRange range;
        std::vector<VariableDef> source_vars;
        std::vector<VariableDef> target_vars;
    };

    struct WindowRecord {
        double start_ts = 0.0;
        double end_ts = 0.0;
        CDiTResult graph;
    };

    struct MergedGroup {
        double start_ts = 0.0;
        double end_ts = 0.0;
        CDiTResult merged_graph;
        std::vector<WindowRecord> originals;
    };

    struct TrialData {
        std::string alias;
        std::string display_name;
        std::string file_path;
        std::vector<WindowRecord> before;
        std::vector<MergedGroup> after_groups;
    };

    struct ExperimentStats {
        int before_count = 0;
        int after_count = 0;
        double elapsed_sec = 0.0;
        double merge_elapsed_sec = 0.0;
        std::map<std::string, int> source_hits;
        std::map<std::string, int> target_hits;
    };

    static std::string trim(const std::string& s) {
        size_t b = 0;
        while (b < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[b]);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++b;
            else break;
        }
        size_t e = s.size();
        while (e > b) {
            unsigned char c = static_cast<unsigned char>(s[e - 1]);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') --e;
            else break;
        }
        return s.substr(b, e - b);
    }

    static bool file_exists(const std::string& path) {
        std::ifstream ifs(path.c_str(), std::ios::binary);
        return ifs.good();
    }

    static void ensure_parent_dir_exists_for_known_logs(void) {
#if defined(_WIN32)
        std::system("if not exist experiment_log mkdir experiment_log >nul 2>nul");
#else
        std::system("mkdir -p experiment_log >/dev/null 2>&1");
#endif
    }

    static std::string strip_utf8_bom(const std::string& s) {
        if (s.size() >= 3 &&
            static_cast<unsigned char>(s[0]) == 0xEF &&
            static_cast<unsigned char>(s[1]) == 0xBB &&
            static_cast<unsigned char>(s[2]) == 0xBF) {
            return s.substr(3);
        }
        return s;
    }

    static std::vector<std::string> parse_csv_line(const std::string& line) {
        std::vector<std::string> cells;
        std::string cur;
        bool in_quotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char ch = line[i];
            if (in_quotes) {
                if (ch == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        cur.push_back('"');
                        ++i;
                    } else {
                        in_quotes = false;
                    }
                } else {
                    cur.push_back(ch);
                }
            } else {
                if (ch == '"') {
                    in_quotes = true;
                } else if (ch == ',') {
                    cells.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(ch);
                }
            }
        }
        cells.push_back(cur);
        return cells;
    }

    static std::string csv_escape(const std::string& s) {
        bool need_quote = false;
        for (size_t i = 0; i < s.size(); ++i) {
            char ch = s[i];
            if (ch == '"' || ch == ',' || ch == '\r' || ch == '\n') {
                need_quote = true;
                break;
            }
        }
        if (!need_quote) return s;
        std::string out = "\"";
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '"') out += "\"\"";
            else out.push_back(s[i]);
        }
        out += '"';
        return out;
    }

    static std::string format_double_2(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << x;
        return oss.str();
    }

    static std::string format_double_4(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4) << x;
        return oss.str();
    }

    static std::vector<std::string> split_multi_delims(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char ch = static_cast<unsigned char>(s[i]);
            if (ch == ',' || ch == ';' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                std::string t = trim(cur);
                if (!t.empty()) out.push_back(t);
                cur.clear();
            } else {
                cur.push_back(static_cast<char>(ch));
            }
        }
        std::string t = trim(cur);
        if (!t.empty()) out.push_back(t);
        return out;
    }

    static QueryRange parse_query_range(const std::string& raw) {
        QueryRange qr;
        std::string t = trim(raw);
        if (t == "=\"-∞到+∞\"" || t == "-∞到+∞") {
            qr.all = true;
            qr.display = "-∞到+∞";
            qr.csv_cell = "=\"-∞到+∞\"";
            return qr;
        }
        qr.csv_cell = t;
        qr.display = t;
        qr.all = false;
        std::string key = t;
        const std::string token = "到";
        size_t pos = key.find(token);
        if (pos == std::string::npos) {
            throw std::runtime_error("查询时间起止范围格式错误：" + raw);
        }
        std::string lhs = trim(key.substr(0, pos));
        std::string rhs = trim(key.substr(pos + token.size()));
        char* endptr = nullptr;
        errno = 0;
        qr.start = std::strtod(lhs.c_str(), &endptr);
        if (errno != 0 || endptr == lhs.c_str() || *endptr != '\0') {
            throw std::runtime_error("查询起始时间无法解析：" + lhs);
        }
        errno = 0;
        endptr = nullptr;
        qr.end = std::strtod(rhs.c_str(), &endptr);
        if (errno != 0 || endptr == rhs.c_str() || *endptr != '\0') {
            throw std::runtime_error("查询结束时间无法解析：" + rhs);
        }
        return qr;
    }

    static std::map<std::string, VariableDef> build_source_dict() {
        std::map<std::string, VariableDef> m;
        m["H"] = {"H", "hatch"};
        m["A1"] = {"A1", "pot_1"};
        m["A2"] = {"A2", "pot_2"};
        m["Lin"] = {"Lin", "load_in"};
        m["Lout"] = {"Lout", "load_out"};
        m["R1"] = {"R1", "v_1"};
        m["R2"] = {"R2", "v_2"};
        m["RM"] = {"RM", "v_mic"};
        m["Rin"] = {"Rin", "v_in"};
        m["Rout"] = {"Rout", "v_out"};
        return m;
    }

    static std::map<std::string, VariableDef> build_target_dict() {
        std::map<std::string, VariableDef> m;
        m["Cin"] = {"Cin", "current_in"};
        m["Cout"] = {"Cout", "current_out"};
        m["ωin"] = {"ωin", "rpm_in"};
        m["ωout"] = {"ωout", "rpm_out"};
        m["Pup"] = {"Pup", "pressure_upwind"};
        m["Pdw"] = {"Pdw", "pressure_downwind"};
        m["Pamb"] = {"Pamb", "pressure_ambient"};
        m["Pint"] = {"Pint", "pressure_intake"};
        m["M"] = {"M", "mic"};
        m["S1"] = {"S1", "signal_1"};
        m["S2"] = {"S2", "signal_2"};
        // 兼容少量 ASCII 写法
        m["win"] = {"ωin", "rpm_in"};
        m["wout"] = {"ωout", "rpm_out"};
        return m;
    }

    static std::vector<VariableDef> parse_variable_list(const std::string& raw,
                                                        const std::map<std::string, VariableDef>& dict,
                                                        const char* kind) {
        std::vector<VariableDef> vars;
        std::vector<std::string> tokens = split_multi_delims(raw);
        std::set<std::string> seen;
        for (size_t i = 0; i < tokens.size(); ++i) {
            std::string t = trim(tokens[i]);
            std::map<std::string, VariableDef>::const_iterator it = dict.find(t);
            if (it == dict.end()) {
                throw std::runtime_error(std::string(kind) + "列中存在未知变量：" + t);
            }
            if (!seen.insert(it->second.short_name).second) continue;
            vars.push_back(it->second);
        }
        if (vars.empty()) {
            throw std::runtime_error(std::string(kind) + "列为空，无法实验。");
        }
        return vars;
    }

    static std::map<std::string, std::pair<std::string, std::string> > build_trial_dict() {
        std::map<std::string, std::pair<std::string, std::string> > m;
        for (int i = 1; i <= 16; ++i) {
            std::ostringstream a, f, d;
            a << 'w' << i;
            f << "actuators_random_walk_" << i << "_pre.csv";
            d << "actuators_random_walk_" << i;
            m[a.str()] = std::make_pair(f.str(), d.str());
        }
        m["w"] = std::make_pair("actuators_random_walk_pre.csv", "actuators_random_walk");
        for (int i = 1; i <= 5; ++i) {
            std::ostringstream a, f, d;
            a << 'f' << i;
            f << "loads_hatch_mix_fast_run_" << i << "_pre.csv";
            d << "loads_hatch_mix_fast_run_" << i;
            m[a.str()] = std::make_pair(f.str(), d.str());
        }
        m["f"] = std::make_pair("loads_hatch_mix_fast_run_pre.csv", "loads_hatch_mix_fast_run");
        for (int i = 1; i <= 5; ++i) {
            std::ostringstream a, f, d;
            a << 's' << i;
            f << "loads_hatch_mix_slow_run_" << i << "_pre.csv";
            d << "loads_hatch_mix_slow_run_" << i;
            m[a.str()] = std::make_pair(f.str(), d.str());
        }
        m["s"] = std::make_pair("loads_hatch_mix_slow_run_pre.csv", "loads_hatch_mix_slow_run");
        m["mul"] = std::make_pair("regime_jumps_multi_pre.csv", "regime_jumps_multi");
        m["sin"] = std::make_pair("regime_jumps_single_pre.csv", "regime_jumps_single");
        return m;
    }

    static std::vector<std::string> parse_trial_aliases(const std::string& raw) {
        std::vector<std::string> xs = split_multi_delims(raw);
        std::set<std::string> seen;
        std::vector<std::string> out;
        std::map<std::string, std::pair<std::string, std::string> > dict = build_trial_dict();
        for (size_t i = 0; i < xs.size(); ++i) {
            if (dict.find(xs[i]) == dict.end()) {
                throw std::runtime_error("指定试验品集合中存在未知试验品：" + xs[i]);
            }
            if (!seen.insert(xs[i]).second) continue;
            out.push_back(xs[i]);
        }
        if (out.empty()) throw std::runtime_error("指定试验品集合为空。");
        return out;
    }

    static ExperimentConfig parse_config_row(const std::vector<std::string>& row) {
        if (row.size() < 9) {
            throw std::runtime_error("实验输入行字段不足 9 列。");
        }
        ExperimentConfig cfg;
        cfg.raw_trial_set = trim(row[0]);
        cfg.raw_query_range = trim(row[1]);
        cfg.raw_source_cols = trim(row[2]);
        cfg.raw_target_cols = trim(row[3]);
        cfg.sample_step = std::atoi(trim(row[4]).c_str());
        cfg.T = std::atoi(trim(row[5]).c_str());
        cfg.tau = std::atoi(trim(row[6]).c_str());
        cfg.Delta = std::atoi(trim(row[7]).c_str());
        cfg.theta = std::atof(trim(row[8]).c_str());
        if (cfg.sample_step <= 0) throw std::runtime_error("采样步长必须为正整数。");
        if (cfg.T <= 0) throw std::runtime_error("完整子随机过程长度 T 必须为正整数。");
        if (cfg.tau <= 0) throw std::runtime_error("τ 必须为正整数。");
        if (cfg.Delta <= 0) throw std::runtime_error("Δ 必须为正整数。");
        if (!(cfg.theta > 0.0 && cfg.theta <= 1.0)) throw std::runtime_error("θ 必须满足 0 < θ <= 1。");

        cfg.trial_aliases = parse_trial_aliases(cfg.raw_trial_set);
        cfg.range = parse_query_range(cfg.raw_query_range);
        cfg.raw_query_range = cfg.range.display;

        const std::map<std::string, VariableDef> source_dict = build_source_dict();
        const std::map<std::string, VariableDef> target_dict = build_target_dict();
        cfg.source_vars = parse_variable_list(cfg.raw_source_cols, source_dict, "参与的原因");
        cfg.target_vars = parse_variable_list(cfg.raw_target_cols, target_dict, "参与的结果");
        return cfg;
    }

    static std::vector<ExperimentConfig> load_experiment_configs() {
        std::ifstream ifs(kInputCsvPath, std::ios::binary);
        if (!ifs.good()) {
            throw std::runtime_error(std::string("无法打开实验输入文件：") + kInputCsvPath);
        }

        std::vector<ExperimentConfig> cfgs;
        std::string line;
        bool first_line = true;
        while (std::getline(ifs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (first_line) {
                line = strip_utf8_bom(line);
                first_line = false;
                continue;
            }
            if (trim(line).empty()) continue;
            std::vector<std::string> row = parse_csv_line(line);
            cfgs.push_back(parse_config_row(row));
        }
        return cfgs;
    }

    static std::string resolve_trial_file_path(const std::string& alias, std::string* display_name) {
        std::map<std::string, std::pair<std::string, std::string> > dict = build_trial_dict();
        std::map<std::string, std::pair<std::string, std::string> >::const_iterator it = dict.find(alias);
        if (it == dict.end()) throw std::runtime_error("未知试验品别名：" + alias);
        if (display_name) *display_name = it->second.second;
        const std::string p1 = std::string(kDataDirPrimary) + it->second.first;
        if (file_exists(p1)) return p1;
        const std::string p2 = std::string(kDataDirFallback) + it->second.first;
        if (file_exists(p2)) return p2;
        return p1;
    }

    struct CsvData {
        std::vector<std::string> header;
        std::vector<std::vector<std::string> > rows;
    };

    static CsvData load_csv_table(const std::string& path) {
        std::ifstream ifs(path.c_str(), std::ios::binary);
        if (!ifs.good()) throw std::runtime_error("无法打开数据文件：" + path);
        CsvData tb;
        std::string line;
        bool first = true;
        while (std::getline(ifs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> cells = parse_csv_line(first ? strip_utf8_bom(line) : line);
            if (first) {
                tb.header = cells;
                first = false;
            } else if (!cells.empty()) {
                tb.rows.push_back(cells);
            }
        }
        return tb;
    }

    static int find_col_index(const std::vector<std::string>& header, const std::string& name) {
        for (size_t i = 0; i < header.size(); ++i) {
            if (trim(header[i]) == name) return (int)i;
        }
        return -1;
    }

    struct FilteredSeries {
        std::vector<double> timestamps;
        std::vector<std::vector<double> > values; // 每行对应一个采样时刻；列依次存放原因变量和结果变量。
    };

    static double parse_double_or_throw(const std::string& s, const std::string& ctx) {
        const std::string ts = trim(s);
        char* endptr = nullptr;
        errno = 0;
        const double v = std::strtod(ts.c_str(), &endptr);
        if (errno != 0 || endptr == ts.c_str() || *endptr != '\0') {
            throw std::runtime_error("无法解析数值(" + ctx + ")：" + s);
        }
        return v;
    }

    static FilteredSeries build_filtered_series(const CsvData& tb, const ExperimentConfig& cfg) {
        const int ts_idx = find_col_index(tb.header, "timestamp");
        if (ts_idx < 0) throw std::runtime_error("数据文件缺少 timestamp 列。");

        std::vector<int> src_idx;
        std::vector<int> tgt_idx;
        for (size_t i = 0; i < cfg.source_vars.size(); ++i) {
            int idx = find_col_index(tb.header, cfg.source_vars[i].csv_name);
            if (idx < 0) throw std::runtime_error("数据文件缺少原因列：" + cfg.source_vars[i].csv_name);
            src_idx.push_back(idx);
        }
        for (size_t i = 0; i < cfg.target_vars.size(); ++i) {
            int idx = find_col_index(tb.header, cfg.target_vars[i].csv_name);
            if (idx < 0) throw std::runtime_error("数据文件缺少结果列：" + cfg.target_vars[i].csv_name);
            tgt_idx.push_back(idx);
        }

        FilteredSeries fs;
        std::vector<double> all_ts;
        std::vector<std::vector<double> > all_vals;
        all_ts.reserve(tb.rows.size());
        all_vals.reserve(tb.rows.size());

        for (size_t r = 0; r < tb.rows.size(); ++r) {
            const std::vector<std::string>& row = tb.rows[r];
            if ((int)row.size() <= ts_idx) continue;
            const double ts = parse_double_or_throw(row[ts_idx], "timestamp");
            if (!cfg.range.all) {
                if (ts < cfg.range.start || ts > cfg.range.end) continue;
            }
            std::vector<double> one;
            one.reserve(cfg.source_vars.size() + cfg.target_vars.size());
            for (size_t i = 0; i < src_idx.size(); ++i) {
                if ((int)row.size() <= src_idx[i]) throw std::runtime_error("原因列索引越界。");
                one.push_back(parse_double_or_throw(row[src_idx[i]], cfg.source_vars[i].csv_name));
            }
            for (size_t i = 0; i < tgt_idx.size(); ++i) {
                if ((int)row.size() <= tgt_idx[i]) throw std::runtime_error("结果列索引越界。");
                one.push_back(parse_double_or_throw(row[tgt_idx[i]], cfg.target_vars[i].csv_name));
            }
            all_ts.push_back(ts);
            all_vals.push_back(one);
        }

        for (size_t i = 0; i < all_ts.size(); i += (size_t)cfg.sample_step) {
            fs.timestamps.push_back(all_ts[i]);
            fs.values.push_back(all_vals[i]);
        }
        return fs;
    }

    static std::string format_graph_ordered(const CDiTResult& r,
                                            const std::vector<VariableDef>& source_vars,
                                            const std::vector<VariableDef>& target_vars) {
        std::ostringstream oss;
        bool first_edge = true;
        for (size_t i = 0; i < source_vars.size(); ++i) {
            const int src_idx = (int)i + 1;
            for (size_t j = 0; j < target_vars.size(); ++j) {
                const int dst_idx = (int)source_vars.size() + (int)j + 1;
                std::map<std::pair<int,int>, std::vector<int> >::const_iterator it =
                        r.lags_by_edge.find(std::make_pair(src_idx, dst_idx));
                if (it == r.lags_by_edge.end() || it->second.empty()) continue;
                if (!first_edge) oss << ' ';
                first_edge = false;
                oss << source_vars[i].short_name << "→" << target_vars[j].short_name << "滞后=";
                std::vector<int> lags = it->second;
                std::sort(lags.begin(), lags.end());
                lags.erase(std::unique(lags.begin(), lags.end()), lags.end());
                for (size_t k = 0; k < lags.size(); ++k) oss << lags[k] << ',';
                oss << ';';
            }
        }
        if (first_edge) return "（空）";
        return oss.str();
    }

    static std::vector<WindowRecord> infer_windows(const FilteredSeries& fs, const ExperimentConfig& cfg) {
        std::vector<WindowRecord> out;
        if (fs.values.empty()) return out;
        const size_t cols = fs.values[0].size();
        if (cols != cfg.source_vars.size() + cfg.target_vars.size()) {
            throw std::runtime_error("参与列数与数据列数不匹配。");
        }
        std::vector<int> source_cols;
        std::vector<int> target_cols;
        for (size_t i = 0; i < cfg.source_vars.size(); ++i) source_cols.push_back((int)i + 1);
        for (size_t i = 0; i < cfg.target_vars.size(); ++i) {
            target_cols.push_back((int)cfg.source_vars.size() + (int)i + 1);
        }

        const size_t full_windows = fs.values.size() / (size_t)cfg.T;
        out.reserve(full_windows);
        for (size_t w = 0; w < full_windows; ++w) {
            const size_t beg = w * (size_t)cfg.T;
            const size_t end = beg + (size_t)cfg.T - 1;
            std::vector<std::vector<double> > sub;
            sub.reserve((size_t)cfg.T);
            for (size_t i = beg; i <= end; ++i) sub.push_back(fs.values[i]);
            WindowRecord rec;
            rec.start_ts = fs.timestamps[beg];
            rec.end_ts = fs.timestamps[end];
            rec.graph = run_cdit(sub, cfg.tau, kDefaultAlpha, false, &source_cols, &target_cols);
            out.push_back(rec);
        }
        return out;
    }

    static MergedGroup make_single_group(const WindowRecord& w) {
        MergedGroup g;
        g.start_ts = w.start_ts;
        g.end_ts = w.end_ts;
        g.merged_graph = w.graph;
        g.originals.push_back(w);
        return g;
    }

    static MergedGroup merge_two_groups(const MergedGroup& left,
                                        const MergedGroup& right,
                                        const CDiTResult& merged_graph) {
        MergedGroup g;
        g.start_ts = left.start_ts;
        g.end_ts = right.end_ts;
        g.merged_graph = merged_graph;
        g.originals.reserve(left.originals.size() + right.originals.size());
        g.originals.insert(g.originals.end(), left.originals.begin(), left.originals.end());
        g.originals.insert(g.originals.end(), right.originals.begin(), right.originals.end());
        return g;
    }

    /*
     * 这里改成“可回退的持续相邻合并”。
     *
     * 旧实现是单次从左到右：
     *   current 与 next 能合并就合并；
     *   一旦不能合并，就立即把 current 固化输出。
     *
     * 这会漏掉一种情况：
     *   A 与 B 先不能合并，
     *   但 B 与 C 合并成 BC 后，
     *   A 又可能与 BC 继续合并。
     *
     * windtunnel_experiment.* / causal_meta 的实际行为更接近：
     * “每形成一个新的相邻段后，都继续检查它与左邻是否还能合并，
     *  直到彻底不能再合并为止。”
     *
     * 因此这里使用栈式归并：
     * 1) 新窗口先压栈；
     * 2) 只要栈顶两个相邻段还能合并，就立刻合并；
     * 3) 合并后继续向左回退检查。
     *
     * 这样可使库外实验的压缩率更贴近库内 windtunnel_experiment.*。
     */
    static std::vector<MergedGroup> merge_windows(const std::vector<WindowRecord>& before,
                                                  const ExperimentConfig& cfg,
                                                  double* merge_elapsed_sec) {
        std::vector<MergedGroup> groups;
        if (before.empty()) return groups;

        groups.reserve(before.size());

        for (size_t i = 0; i < before.size(); ++i) {
            groups.push_back(make_single_group(before[i]));

            while (groups.size() >= 2) {
                const MergedGroup left = groups[groups.size() - 2];
                const MergedGroup right = groups[groups.size() - 1];

                const double t0 = std::chrono::duration_cast<std::chrono::duration<double> >(
                        std::chrono::steady_clock::now().time_since_epoch()).count();

                MergeCGResult mg = merge_adjacent_causal_graphs(left.merged_graph,
                                                                right.merged_graph,
                                                                cfg.tau,
                                                                cfg.Delta,
                                                                cfg.theta,
                                                                true);

                const double t1 = std::chrono::duration_cast<std::chrono::duration<double> >(
                        std::chrono::steady_clock::now().time_since_epoch()).count();

                if (merge_elapsed_sec) *merge_elapsed_sec += (t1 - t0);

                if (mg.merge != 1) break;

                MergedGroup merged = merge_two_groups(left, right, mg.merged);
                groups.pop_back();
                groups.pop_back();
                groups.push_back(std::move(merged));
            }
        }

        return groups;
    }

    static void update_variable_frequency(const std::vector<WindowRecord>& before,
                                          const ExperimentConfig& cfg,
                                          ExperimentStats* stats) {
        for (size_t i = 0; i < before.size(); ++i) {
            ++stats->before_count;
            std::set<int> hit_src;
            std::set<int> hit_tgt;
            for (std::map<std::pair<int,int>, std::vector<int> >::const_iterator it = before[i].graph.lags_by_edge.begin();
                 it != before[i].graph.lags_by_edge.end(); ++it) {
                if (it->second.empty()) continue;
                int src = it->first.first;
                int dst = it->first.second;
                if (src >= 1 && src <= (int)cfg.source_vars.size()) hit_src.insert(src - 1);
                const int tbase = (int)cfg.source_vars.size() + 1;
                if (dst >= tbase && dst < tbase + (int)cfg.target_vars.size()) hit_tgt.insert(dst - tbase);
            }
            for (std::set<int>::const_iterator it = hit_src.begin(); it != hit_src.end(); ++it) {
                ++stats->source_hits[cfg.source_vars[*it].short_name];
            }
            for (std::set<int>::const_iterator it = hit_tgt.begin(); it != hit_tgt.end(); ++it) {
                ++stats->target_hits[cfg.target_vars[*it].short_name];
            }
        }
    }

    static std::string format_window_line(const WindowRecord& w, const ExperimentConfig& cfg) {
        std::ostringstream oss;
        oss << format_double_2(w.start_ts) << "到" << format_double_2(w.end_ts)
            << " | 推断：" << format_graph_ordered(w.graph, cfg.source_vars, cfg.target_vars);
        return oss.str();
    }

    static std::string format_group_line(const MergedGroup& g, const ExperimentConfig& cfg) {
        std::ostringstream oss;
        oss << format_double_2(g.start_ts) << "到" << format_double_2(g.end_ts)
            << " | 推断：" << format_graph_ordered(g.merged_graph, cfg.source_vars, cfg.target_vars);
        return oss.str();
    }

    static std::string build_trial_output(const TrialData& td, const ExperimentConfig& cfg) {
        std::ostringstream oss;
        oss << "试验品：" << td.display_name << "\n";

        if (td.before.empty()) {
            if (kPrintBefore) oss << "合并前\n（无完整子随机过程）\n";
            if (kPrintAfter) oss << "合并后\n（无完整子随机过程）\n";
            if (kPrintCompare) oss << "合并前后效果对比\n（无完整子随机过程）\n";
            return oss.str();
        }

        if (kPrintBefore) {
            oss << "合并前\n";
            for (size_t i = 0; i < td.before.size(); ++i) {
                oss << format_window_line(td.before[i], cfg) << "\n";
            }
        }

        if (kPrintAfter) {
            oss << "合并后\n";
            for (size_t i = 0; i < td.after_groups.size(); ++i) {
                oss << format_group_line(td.after_groups[i], cfg) << "\n";
            }
        }

        if (kPrintCompare) {
            oss << "合并前后效果对比\n";
            for (size_t i = 0; i < td.after_groups.size(); ++i) {
                const MergedGroup& g = td.after_groups[i];
                if (g.originals.size() == 1) {
                    oss << format_window_line(g.originals[0], cfg) << "\n";
                } else {
                    for (size_t j = 0; j < g.originals.size(); ++j) {
                        if (j == 0) oss << "┌";
                        else oss << "├";
                        oss << format_window_line(g.originals[j], cfg) << "\n";
                    }
                    oss << "└▷" << format_group_line(g, cfg) << "\n";
                }
            }
        }

        return oss.str();
    }

    static std::vector<std::pair<std::string, double> > sort_frequency_items(const std::map<std::string, int>& counts,
                                                                             int denom,
                                                                             const std::vector<VariableDef>& order_hint) {
        std::map<std::string, int> order;
        for (size_t i = 0; i < order_hint.size(); ++i) order[order_hint[i].short_name] = (int)i;

        std::vector<std::pair<std::string, double> > items;
        for (std::map<std::string, int>::const_iterator it = counts.begin(); it != counts.end(); ++it) {
            double pct = (denom <= 0) ? 0.0 : (100.0 * (double)it->second / (double)denom);
            items.push_back(std::make_pair(it->first, pct));
        }
        std::sort(items.begin(), items.end(), [&](const std::pair<std::string, double>& a,
                                                  const std::pair<std::string, double>& b) {
            if (std::fabs(a.second - b.second) > 1e-12) return a.second > b.second;
            int oa = order.count(a.first) ? order[a.first] : 1000000;
            int ob = order.count(b.first) ? order[b.first] : 1000000;
            if (oa != ob) return oa < ob;
            return a.first < b.first;
        });
        return items;
    }

    static std::string format_frequency_ranking(const std::map<std::string, int>& counts,
                                                int denom,
                                                const std::vector<VariableDef>& order_hint) {
        std::vector<std::pair<std::string, double> > items = sort_frequency_items(counts, denom, order_hint);
        if (items.empty()) return "（空）";
        std::ostringstream oss;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i) oss << ',';
            oss << items[i].first << '(' << format_double_2(items[i].second) << "%)";
        }
        return oss.str();
    }

    static std::string build_experiment_header(int idx, const ExperimentConfig& cfg) {
        std::ostringstream oss;
        oss << "风洞实验第" << idx << "次实验\n";
        oss << "本次数据库外纯CDiT推断+Merge实验参数设置："
            << "试验品=" << cfg.raw_trial_set
            << "；因果查询起止=" << cfg.range.display
            << "；参与的原因列=" << cfg.raw_source_cols
            << "；参与的结果列=" << cfg.raw_target_cols
            << "；采样步长=" << cfg.sample_step
            << "；T=" << cfg.T
            << "；τ=" << cfg.tau
            << "；Δ=" << cfg.Delta
            << "；θ=" << cfg.theta << "\n";
        return oss.str();
    }

    static std::string build_experiment_footer(const ExperimentConfig& cfg, const ExperimentStats& stats) {
        const double compress = (stats.before_count <= 0)
                                ? 0.0
                                : (100.0 * (1.0 - (double)stats.after_count / (double)stats.before_count));
        std::ostringstream oss;
        oss << "小结：耗时:" << format_double_2(stats.elapsed_sec) << "s，合并前=" << stats.before_count
            << "，合并后=" << stats.after_count
            << "，压缩率=" << format_double_2(compress) << "%\n";
        oss << "变量频率排序：原因变量中"
            << format_frequency_ranking(stats.source_hits, stats.before_count, cfg.source_vars)
            << " | 结果变量中"
            << format_frequency_ranking(stats.target_hits, stats.before_count, cfg.target_vars)
            << "\n\n";
        return oss.str();
    }

    static void append_text_file(const std::string& path, const std::string& content) {
        std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::app);
        if (!ofs.good()) throw std::runtime_error("无法写入日志文件：" + path);
        ofs.write(content.data(), (std::streamsize)content.size());
    }

    static void reset_text_file_with_bom(const std::string& path) {
        std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!ofs.good()) throw std::runtime_error("无法重置日志文件：" + path);
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        ofs.write(reinterpret_cast<const char*>(bom), 3);
    }

    static std::string build_stat_csv_header() {
        return std::string(
                "指定试验品集合,查询时间起止范围,参与的原因列,参与的结果列,采样步长,完整子随机过程长度T,推断时的τ,合并时的Δ,合并时的θ,耗时(s),压缩率,原因变量频率排序,结果变量频率排序,仅合并耗时(s)\r\n");
    }

    static std::string build_stat_csv_row(const ExperimentConfig& cfg, const ExperimentStats& stats) {
        const double compress = (stats.before_count <= 0)
                                ? 0.0
                                : (100.0 * (1.0 - (double)stats.after_count / (double)stats.before_count));
        std::vector<std::string> cells;
        cells.push_back(cfg.raw_trial_set);
        cells.push_back(cfg.range.csv_cell.empty() ? cfg.raw_query_range : cfg.range.csv_cell);
        cells.push_back(cfg.raw_source_cols);
        cells.push_back(cfg.raw_target_cols);
        cells.push_back(std::to_string(cfg.sample_step));
        cells.push_back(std::to_string(cfg.T));
        cells.push_back(std::to_string(cfg.tau));
        cells.push_back(std::to_string(cfg.Delta));
        {
            std::ostringstream oss;
            oss << cfg.theta;
            cells.push_back(oss.str());
        }
        cells.push_back(format_double_2(stats.elapsed_sec));
        cells.push_back(format_double_2(compress) + "%");
        cells.push_back(format_frequency_ranking(stats.source_hits, stats.before_count, cfg.source_vars));
        cells.push_back(format_frequency_ranking(stats.target_hits, stats.before_count, cfg.target_vars));
        cells.push_back(format_double_4(stats.merge_elapsed_sec));

        std::ostringstream oss;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (i) oss << ',';
            oss << csv_escape(cells[i]);
        }
        oss << "\r\n";
        return oss.str();
    }

    static void init_stat_csv() {
        std::ofstream ofs(kStatCsvPath, std::ios::binary | std::ios::trunc);
        if (!ofs.good()) throw std::runtime_error(std::string("无法创建统计 CSV：") + kStatCsvPath);
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        ofs.write(reinterpret_cast<const char*>(bom), 3);
        std::string header = build_stat_csv_header();
        ofs.write(header.data(), (std::streamsize)header.size());
    }

    static void append_stat_csv_row(const std::string& row) {
        std::ofstream ofs(kStatCsvPath, std::ios::binary | std::ios::app);
        if (!ofs.good()) throw std::runtime_error(std::string("无法追加统计 CSV：") + kStatCsvPath);
        ofs.write(row.data(), (std::streamsize)row.size());
    }

    // 执行一个 WindTunnel 试验品的窗口推断与相邻窗口合并，并收集图规模和耗时统计。
    static TrialData run_one_trial(const std::string& alias, const ExperimentConfig& cfg, ExperimentStats* stats) {
        TrialData td;
        td.alias = alias;
        td.file_path = resolve_trial_file_path(alias, &td.display_name);
        CsvData tb = load_csv_table(td.file_path);
        FilteredSeries fs = build_filtered_series(tb, cfg);
        td.before = infer_windows(fs, cfg);
        td.after_groups = merge_windows(td.before, cfg, &stats->merge_elapsed_sec);
        update_variable_frequency(td.before, cfg, stats);
        stats->after_count += (int)td.after_groups.size();
        return td;
    }

    static double now_seconds() {
        return std::chrono::duration_cast<std::chrono::duration<double> >(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    }

} // 匿名命名空间结束

// WindTunnel 窗口合并实验入口：逐项运行配置中的试验品并输出汇总结果。
extern "C" int run_cdit_merge_windtunnel_experiments(void) {
    try {
        ensure_parent_dir_exists_for_known_logs();
        reset_text_file_with_bom(kPrintLogPath);
        init_stat_csv();

        std::vector<ExperimentConfig> cfgs = load_experiment_configs();
        for (size_t i = 0; i < cfgs.size(); ++i) {
            const ExperimentConfig& cfg = cfgs[i];
            ExperimentStats stats;
            const double t0 = now_seconds();

            std::ostringstream experiment_out;
            experiment_out << build_experiment_header((int)i + 1, cfg);

            for (size_t j = 0; j < cfg.trial_aliases.size(); ++j) {
                TrialData td = run_one_trial(cfg.trial_aliases[j], cfg, &stats);
                experiment_out << build_trial_output(td, cfg);
            }

            stats.elapsed_sec = now_seconds() - t0;
            experiment_out << build_experiment_footer(cfg, stats);

            const std::string text = experiment_out.str();
            std::printf("%s", text.c_str());
            std::fflush(stdout);
            append_text_file(kPrintLogPath, text);
            append_stat_csv_row(build_stat_csv_row(cfg, stats));
        }
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "run_cdit_merge_windtunnel_experiments error: %s\n", ex.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "run_cdit_merge_windtunnel_experiments error: unknown exception\n");
        return 1;
    }
}
