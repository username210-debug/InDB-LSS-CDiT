/*
 * 本文件实现 WindTunnel 数据集的库外处理对照实验，负责数据加载、窗口分析、结果组织和耗时统计。
 */

#include "outdb_windtunnel_experiment.h"

#include "../xdb.h"
#include "../cdit_module.h"
#include "../merge_cg.h"
#include "../causal_meta.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

    struct Timer {
        std::chrono::steady_clock::time_point t0;
        Timer() : t0(std::chrono::steady_clock::now()) {}
        void reset() { t0 = std::chrono::steady_clock::now(); }
        double seconds() const {
            const auto t1 = std::chrono::steady_clock::now();
            return std::chrono::duration<double>(t1 - t0).count();
        }
    };

    struct ScopedAccumulator {
        Timer timer;
        double *acc;
        explicit ScopedAccumulator(double *target) : acc(target) {}
        ~ScopedAccumulator() { if (acc) *acc += timer.seconds(); }
    };

    struct TeeLogger {
        std::ofstream file;

        explicit TeeLogger(const std::string& path) {
            file.open(path, std::ios::binary | std::ios::trunc);
            if (file.good()) {
                const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
                file.write(reinterpret_cast<const char*>(bom), 3);
            }
        }

        void write(const std::string& s) {
            std::cout << s;
            std::cout.flush();
            if (file.good()) {
                file << s;
                file.flush();
            }
        }

        void line(const std::string& s = std::string()) {
            write(s + "\n");
        }
    };

    struct ExperimentConfig {
        std::string sample_spec;
        std::string range_text;
        std::string cause_spec;
        std::string effect_spec;
        int sampling_step = 1;
        int window_length = 1000;
        int tau = 5;
        int delta = 2;
        double theta = 0.8;
        int memtable_mb = 2;
        bool use_covmeta_cdit = true;
    };

    struct QueryRange {
        double start = -std::numeric_limits<double>::infinity();
        double end = std::numeric_limits<double>::infinity();
    };

    struct DatasetSpec {
        std::string token;
        std::string base_name;
        std::string file_path;
    };

    struct ColumnSelection {
        std::vector<std::string> cause_aliases;
        std::vector<std::string> effect_aliases;
        std::vector<std::string> all_aliases;
        std::vector<int> selected_value_indices;
        std::vector<int> source_columns;
        std::vector<int> target_columns;
    };

    struct TimestampRef {
        std::string key;
        std::string label;
        double value = 0.0;
    };

    struct LoadedDataset {
        DatasetSpec spec;
        ColumnSelection columns;
        std::vector<TimestampRef> timestamps;
    };

    struct Segment {
        std::string start_label;
        std::string end_label;
        CDiTResult graph;
    };

    static const std::string kInputCsv = "experiment/outdb_windtunnel_experiment_input.csv";
    static const std::string kDataDir = "data/windtunnel_data";
    static const std::string kDbDir = "xdb_windtunnel_outdb_experiment";
    static const std::string kLogDir = "experiment_log";
    static const std::string kStatsCsv = "experiment_log/对风洞实验的入库+库外推断实验统计.csv";
    static const std::string kPrintLog = "experiment_log/风洞实验入库+库外推断实验全部打印日志.txt";

    static std::string trim(const std::string& s) {
        size_t l = 0;
        size_t r = s.size();
        while (l < r && static_cast<unsigned char>(s[l]) <= 0x20) ++l;
        while (r > l && static_cast<unsigned char>(s[r - 1]) <= 0x20) --r;
        return s.substr(l, r - l);
    }

    static std::string remove_utf8_bom(std::string s) {
        if (s.size() >= 3 &&
            static_cast<unsigned char>(s[0]) == 0xEF &&
            static_cast<unsigned char>(s[1]) == 0xBB &&
            static_cast<unsigned char>(s[2]) == 0xBF) {
            return s.substr(3);
        }
        return s;
    }

    static std::vector<std::string> parse_csv_line(const std::string& line) {
        std::vector<std::string> out;
        std::string cur;
        bool in_quote = false;
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (in_quote) {
                if (c == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        cur.push_back('"');
                        ++i;
                    } else {
                        in_quote = false;
                    }
                } else {
                    cur.push_back(c);
                }
            } else {
                if (c == '"') {
                    in_quote = true;
                } else if (c == ',') {
                    out.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
        }
        out.push_back(cur);
        return out;
    }

    static std::string csv_escape(const std::string& s) {
        const bool need_quote = s.find_first_of(",\"\r\n") != std::string::npos;
        if (!need_quote) return s;
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\"\"";
            else out.push_back(c);
        }
        out.push_back('"');
        return out;
    }

    static std::string csv_excel_text(const std::string& s) {
        if (s == "-∞到+∞") return "\"=\"\"-∞到+∞\"\"\"";
        return csv_escape(s);
    }

    static void write_utf8_bom(std::ofstream& out) {
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        out.write(reinterpret_cast<const char*>(bom), 3);
    }

    static std::string normalize_excel_text(std::string s) {
        s = trim(remove_utf8_bom(s));
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            s = s.substr(1, s.size() - 2);
        }
        s = trim(s);
        if (!s.empty() && s[0] == '=') {
            s = trim(s.substr(1));
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                s = s.substr(1, s.size() - 2);
            }
        }
        return trim(s);
    }

    static std::string lower_ascii(std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    }

    static bool parse_bool_zh(const std::string& raw, bool default_value) {
        std::string s = normalize_excel_text(raw);
        std::string l = lower_ascii(s);
        if (s == "是" || s == "真" || s == "使用" || s == "协方差" || s == "协方差缓存" ||
            l == "1" || l == "true" || l == "yes" || l == "y" || l == "on" ||
            l == "cov" || l == "covmeta" || l == "covariance") {
            return true;
        }
        if (s == "否" || s == "假" || s == "不使用" || s == "原始" ||
            l == "0" || l == "false" || l == "no" || l == "n" || l == "off" ||
            l == "original" || l == "origin") {
            return false;
        }
        return default_value;
    }

    static std::string bool_zh(bool x) {
        return x ? "是" : "否";
    }

    static std::string format_double2(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << x;
        return oss.str();
    }

    static std::string format_seconds(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4) << x;
        return oss.str();
    }

    static std::string format_theta(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << x;
        return oss.str();
    }

    static std::string format_percent(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << x << "%";
        return oss.str();
    }

    static std::string join_strings(const std::vector<std::string>& xs, const std::string& sep) {
        std::ostringstream oss;
        for (size_t i = 0; i < xs.size(); ++i) {
            if (i) oss << sep;
            oss << xs[i];
        }
        return oss.str();
    }

    static std::vector<std::string> split_list(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        auto flush = [&]() {
            std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        };
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n') flush();
            else cur.push_back(static_cast<char>(c));
        }
        flush();
        return out;
    }

    static double parse_timestamp_double(const std::string& s) {
        char *end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str()) return 0.0;
        return v;
    }

    static double parse_bound(const std::string& raw, bool left) {
        std::string s = trim(raw);
        const std::string sl = lower_ascii(s);
        if (s.find("∞") != std::string::npos) {
            if (s.find('-') != std::string::npos) return -std::numeric_limits<double>::infinity();
            return std::numeric_limits<double>::infinity();
        }
        if (sl == "-inf" || sl == "-infinity") return -std::numeric_limits<double>::infinity();
        if (sl == "+inf" || sl == "inf" || sl == "+infinity" || sl == "infinity") return std::numeric_limits<double>::infinity();
        if (s.empty()) return left ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return std::strtod(s.c_str(), nullptr);
    }

    static QueryRange parse_query_range(const std::string& range_text) {
        QueryRange qr;
        std::string s = normalize_excel_text(range_text);
        if (s.empty() || s == "-∞到+∞") return qr;

        size_t pos = s.find("到");
        size_t sep_len = 3;
        if (pos == std::string::npos) {
            pos = s.find('~');
            sep_len = 1;
        }
        if (pos == std::string::npos) {
            pos = s.find('-');
            sep_len = 1;
        }
        if (pos == std::string::npos) {
            qr.start = parse_bound(s, true);
            qr.end = qr.start;
            return qr;
        }
        qr.start = parse_bound(s.substr(0, pos), true);
        qr.end = parse_bound(s.substr(pos + sep_len), false);
        if (qr.start > qr.end) std::swap(qr.start, qr.end);
        return qr;
    }

    static bool in_range(double x, const QueryRange& r) {
        return x >= r.start && x <= r.end;
    }

    static std::map<std::string, std::string> alias_to_header_name() {
        return {
                {"H", "hatch"},
                {"A1", "pot_1"},
                {"A2", "pot_2"},
                {"Lin", "load_in"},
                {"Lout", "load_out"},
                {"R1", "v_1"},
                {"R2", "v_2"},
                {"RM", "v_mic"},
                {"Rin", "v_in"},
                {"Rout", "v_out"},
                {"Cin", "current_in"},
                {"Cout", "current_out"},
                {"ωin", "rpm_in"},
                {"win", "rpm_in"},
                {"Win", "rpm_in"},
                {"omega_in", "rpm_in"},
                {"rpm_in", "rpm_in"},
                {"ωout", "rpm_out"},
                {"wout", "rpm_out"},
                {"Wout", "rpm_out"},
                {"omega_out", "rpm_out"},
                {"rpm_out", "rpm_out"},
                {"Pup", "pressure_upwind"},
                {"Pdw", "pressure_downwind"},
                {"Pamb", "pressure_ambient"},
                {"Pint", "pressure_intake"},
                {"M", "mic"},
                {"S1", "signal_1"},
                {"S2", "signal_2"}
        };
    }

    static std::string canonical_display_alias(const std::string& alias) {
        if (alias == "win" || alias == "Win" || alias == "omega_in" || alias == "rpm_in") return "ωin";
        if (alias == "wout" || alias == "Wout" || alias == "omega_out" || alias == "rpm_out") return "ωout";
        return alias;
    }

    static std::string sample_token_to_base(const std::string& token) {
        const std::string t = trim(token);
        if (t == "mul") return "regime_jumps_multi";
        if (t == "sin") return "regime_jumps_single";
        if (t == "w") return "actuators_random_walk";
        if (t == "f") return "loads_hatch_mix_fast_run";
        if (t == "s") return "loads_hatch_mix_slow_run";
        if (t.size() >= 2 && t[0] == 'w') {
            int n = std::atoi(t.c_str() + 1);
            if (n >= 1 && n <= 16) return "actuators_random_walk_" + std::to_string(n);
        }
        if (t.size() >= 2 && t[0] == 'f') {
            int n = std::atoi(t.c_str() + 1);
            if (n >= 1 && n <= 5) return "loads_hatch_mix_fast_run_" + std::to_string(n);
        }
        if (t.size() >= 2 && t[0] == 's') {
            int n = std::atoi(t.c_str() + 1);
            if (n >= 1 && n <= 5) return "loads_hatch_mix_slow_run_" + std::to_string(n);
        }
        std::string base = t;
        const std::string suffix = "_pre.csv";
        if (base.size() > suffix.size() && base.substr(base.size() - suffix.size()) == suffix) base = base.substr(0, base.size() - suffix.size());
        else if (base.size() > 4 && base.substr(base.size() - 4) == ".csv") base = base.substr(0, base.size() - 4);
        else if (base.size() > 4 && base.substr(base.size() - 4) == "_pre") base = base.substr(0, base.size() - 4);
        return base;
    }

    static std::vector<DatasetSpec> all_dataset_specs() {
        std::vector<DatasetSpec> specs;
        for (int i = 1; i <= 16; ++i) specs.push_back({"w" + std::to_string(i), "actuators_random_walk_" + std::to_string(i), ""});
        specs.push_back({"w", "actuators_random_walk", ""});
        for (int i = 1; i <= 5; ++i) specs.push_back({"f" + std::to_string(i), "loads_hatch_mix_fast_run_" + std::to_string(i), ""});
        specs.push_back({"f", "loads_hatch_mix_fast_run", ""});
        for (int i = 1; i <= 5; ++i) specs.push_back({"s" + std::to_string(i), "loads_hatch_mix_slow_run_" + std::to_string(i), ""});
        specs.push_back({"s", "loads_hatch_mix_slow_run", ""});
        specs.push_back({"mul", "regime_jumps_multi", ""});
        specs.push_back({"sin", "regime_jumps_single", ""});
        return specs;
    }

    static std::string data_path_for_base(const std::string& base) {
        const std::string p1 = kDataDir + "/" + base + "_pre.csv";
        if (std::filesystem::exists(p1)) return p1;
        const std::string p2 = base + "_pre.csv";
        if (std::filesystem::exists(p2)) return p2;
        const std::string p3 = kDataDir + "/" + base + ".csv";
        if (std::filesystem::exists(p3)) return p3;
        return p1;
    }

    static std::vector<DatasetSpec> expand_sample_spec(const std::string& sample_spec) {
        std::vector<DatasetSpec> specs;
        std::set<std::string> seen;
        const std::string normalized = lower_ascii(trim(sample_spec));
        if (normalized == "all" || normalized == "全部" || normalized == "*") {
            specs = all_dataset_specs();
        } else {
            for (const std::string& tok : split_list(sample_spec)) {
                const std::string base = sample_token_to_base(tok);
                if (base.empty() || !seen.insert(base).second) continue;
                specs.push_back({tok, base, ""});
            }
        }
        for (DatasetSpec& s : specs) s.file_path = data_path_for_base(s.base_name);
        return specs;
    }

    static bool ensure_default_input_if_missing() {
        if (std::filesystem::exists(kInputCsv)) return true;
        std::filesystem::create_directories("experiment");
        std::ofstream out(kInputCsv, std::ios::binary | std::ios::trunc);
        if (!out.good()) return false;
        write_utf8_bom(out);
        out << "指定试验品集合,查询时间起止范围,参与的原因列,参与的结果列,采样步长,完整子随机过程长度T,推断时的τ,合并时的Δ,合并时的θ,MemTable容量(MB),是否使用协方差缓存驱动CDiT\n";
        out << "mul sin,\"=\"\"-∞到+∞\"\"\",\"Lin,Lout\",\"ωout,Pdw,ωin,Cout,Cin,Pup\",1,1000,5,2,0.8,2,是\n";
        return true;
    }

    // 读取 WindTunnel 库外对照实验配置，并校验试验品、窗口及推断参数。
    static std::vector<ExperimentConfig> read_experiment_configs(TeeLogger& log) {
        std::vector<ExperimentConfig> configs;
        if (!ensure_default_input_if_missing()) {
            log.line("无法创建默认实验输入文件：" + kInputCsv);
            return configs;
        }

        std::ifstream in(kInputCsv, std::ios::binary);
        if (!in.good()) {
            log.line("无法打开实验输入文件：" + kInputCsv);
            return configs;
        }

        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (trim(remove_utf8_bom(line)).empty()) continue;
            if (first) { first = false; continue; }
            std::vector<std::string> f = parse_csv_line(line);
            if (f.size() < 10) continue;
            ExperimentConfig cfg;
            cfg.sample_spec = normalize_excel_text(f[0]);
            cfg.range_text = normalize_excel_text(f[1]);
            cfg.cause_spec = normalize_excel_text(f[2]);
            cfg.effect_spec = normalize_excel_text(f[3]);
            cfg.sampling_step = std::max(1, std::atoi(normalize_excel_text(f[4]).c_str()));
            cfg.window_length = std::max(1, std::atoi(normalize_excel_text(f[5]).c_str()));
            cfg.tau = std::max(1, std::atoi(normalize_excel_text(f[6]).c_str()));
            cfg.delta = std::max(1, std::atoi(normalize_excel_text(f[7]).c_str()));
            cfg.theta = std::strtod(normalize_excel_text(f[8]).c_str(), nullptr);
            cfg.memtable_mb = std::max(1, std::atoi(normalize_excel_text(f[9]).c_str()));
            cfg.use_covmeta_cdit = (f.size() >= 11) ? parse_bool_zh(f[10], true) : true;
            if (cfg.range_text.empty()) cfg.range_text = "-∞到+∞";
            configs.push_back(cfg);
        }
        return configs;
    }

    static bool build_column_selection(const std::vector<std::string>& header,
                                       const std::string& cause_spec,
                                       const std::string& effect_spec,
                                       ColumnSelection& out,
                                       std::string& error) {
        std::map<std::string, int> header_index;
        for (size_t i = 0; i < header.size(); ++i) header_index[trim(header[i])] = static_cast<int>(i);
        const auto alias_map = alias_to_header_name();

        auto resolve_alias = [&](const std::string& raw_alias, int& value_idx, std::string& display_alias) -> bool {
            display_alias = canonical_display_alias(trim(raw_alias));
            std::string header_name;
            auto ait = alias_map.find(trim(raw_alias));
            if (ait != alias_map.end()) header_name = ait->second;
            else header_name = trim(raw_alias);
            auto hit = header_index.find(header_name);
            if (hit == header_index.end()) return false;
            if (hit->second <= 0) return false;
            value_idx = hit->second - 1;
            return true;
        };

        out = ColumnSelection();
        std::set<std::string> seen_display;
        for (const std::string& raw : split_list(cause_spec)) {
            int idx = -1;
            std::string display;
            if (!resolve_alias(raw, idx, display)) { error = "未知原因列：" + raw; return false; }
            if (!seen_display.insert("S:" + display).second) continue;
            out.cause_aliases.push_back(display);
            out.selected_value_indices.push_back(idx);
            out.all_aliases.push_back(display);
        }
        for (const std::string& raw : split_list(effect_spec)) {
            int idx = -1;
            std::string display;
            if (!resolve_alias(raw, idx, display)) { error = "未知结果列：" + raw; return false; }
            if (!seen_display.insert("T:" + display).second) continue;
            out.effect_aliases.push_back(display);
            out.selected_value_indices.push_back(idx);
            out.all_aliases.push_back(display);
        }
        if (out.cause_aliases.empty() || out.effect_aliases.empty()) {
            error = "原因列或结果列为空";
            return false;
        }
        for (size_t i = 0; i < out.cause_aliases.size(); ++i) out.source_columns.push_back(static_cast<int>(i) + 1);
        for (size_t i = 0; i < out.effect_aliases.size(); ++i) out.target_columns.push_back(static_cast<int>(out.cause_aliases.size() + i + 1));
        return true;
    }

    static std::string make_value_string_from_fields(const std::vector<std::string>& f) {
        std::ostringstream oss;
        for (size_t i = 1; i < f.size(); ++i) {
            if (i > 1) oss << ',';
            oss << trim(f[i]);
        }
        return oss.str();
    }

    static bool parse_selected_values(const std::string& value,
                                      const std::vector<int>& selected_indices,
                                      std::vector<double>& row) {
        const std::vector<std::string> vals = parse_csv_line(value);
        row.clear();
        row.reserve(selected_indices.size());
        for (int idx : selected_indices) {
            if (idx < 0 || idx >= static_cast<int>(vals.size())) return false;
            row.push_back(std::strtod(trim(vals[static_cast<size_t>(idx)]).c_str(), nullptr));
        }
        return true;
    }

    // 将一个风洞试验品的有效记录写入 RemixDB，并维护其对应的数据库键值。
    static bool insert_one_dataset(struct xdb_ref* ref,
                                   const DatasetSpec& spec,
                                   const ExperimentConfig& cfg,
                                   LoadedDataset& loaded,
                                   TeeLogger& log) {
        (void)cfg;
        std::ifstream in(spec.file_path, std::ios::binary);
        if (!in.good()) {
            log.line("警告：找不到数据文件，跳过试验品 " + spec.base_name + "：" + spec.file_path);
            return false;
        }

        std::string header_line;
        if (!std::getline(in, header_line)) return false;
        if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
        std::vector<std::string> header = parse_csv_line(remove_utf8_bom(header_line));

        ColumnSelection columns;
        std::string error;
        if (!build_column_selection(header, cfg.cause_spec, cfg.effect_spec, columns, error)) {
            log.line("警告：" + spec.base_name + " 的列解析失败：" + error);
            return false;
        }

        loaded = LoadedDataset();
        loaded.spec = spec;
        loaded.columns = columns;

        std::string line;
        uint64_t row_count = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> fields = parse_csv_line(line);
            if (fields.size() < 2) continue;
            const std::string ts_key = trim(fields[0]);
            if (ts_key.empty()) continue;
            const double ts_value = parse_timestamp_double(ts_key);
            const std::string label = format_double2(ts_value);
            const std::string key = spec.base_name + "," + ts_key;
            const std::string value = make_value_string_from_fields(fields);
            bool ok = remixdb_put(ref, key.data(), static_cast<u32>(key.size()), value.data(), static_cast<u32>(value.size()));
            if (!ok) {
                log.line("警告：写入失败，key=" + key);
                continue;
            }
            loaded.timestamps.push_back({ts_key, label, ts_value});
            ++row_count;
        }

        if (row_count == 0) {
            log.line("警告：" + spec.base_name + " 没有可插入的数据行");
            return false;
        }
        return true;
    }

    static std::string format_graph_ordered(const CDiTResult& graph,
                                            const std::vector<std::string>& names,
                                            const std::vector<std::string>& source_order,
                                            const std::vector<std::string>& target_order) {
        std::map<std::string, int> source_rank;
        std::map<std::string, int> target_rank;
        for (size_t i = 0; i < source_order.size(); ++i) source_rank[source_order[i]] = static_cast<int>(i);
        for (size_t i = 0; i < target_order.size(); ++i) target_rank[target_order[i]] = static_cast<int>(i);

        auto name_of = [&](int idx) -> std::string {
            if (idx >= 1 && idx <= static_cast<int>(names.size())) return names[static_cast<size_t>(idx - 1)];
            return "X" + std::to_string(idx);
        };

        struct Item {
            int sr = 0;
            int tr = 0;
            int p = 0;
            int q = 0;
            std::string ps;
            std::string qs;
            std::vector<int> lags;
        };

        std::vector<Item> items;
        for (const auto& kv : graph.lags_by_edge) {
            if (kv.second.empty()) continue;
            const std::string ps = name_of(kv.first.first);
            const std::string qs = name_of(kv.first.second);
            int sr = source_rank.count(ps) ? source_rank[ps] : 100000 + kv.first.first;
            int tr = target_rank.count(qs) ? target_rank[qs] : 100000 + kv.first.second;
            items.push_back({sr, tr, kv.first.first, kv.first.second, ps, qs, kv.second});
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.sr != b.sr) return a.sr < b.sr;
            if (a.tr != b.tr) return a.tr < b.tr;
            if (a.p != b.p) return a.p < b.p;
            return a.q < b.q;
        });

        std::ostringstream oss;
        for (const Item& item : items) {
            std::vector<int> lags = item.lags;
            std::sort(lags.begin(), lags.end());
            oss << item.ps << "→" << item.qs << "滞后=";
            for (int v : lags) oss << v << ',';
            oss << "; ";
        }
        return oss.str();
    }

    static bool read_one_row_from_db(struct xdb_ref* ref,
                                     const LoadedDataset& ds,
                                     const TimestampRef& ts,
                                     std::vector<double>& row) {
        const std::string key = ds.spec.base_name + "," + ts.key;
        std::vector<char> buf(65536, 0);
        u32 vlen = 0;
        bool ok = remixdb_get(ref, key.data(), static_cast<u32>(key.size()), buf.data(), &vlen);
        if (!ok) return false;
        if (vlen >= buf.size()) return false;
        std::string value(buf.data(), buf.data() + vlen);
        return parse_selected_values(value, ds.columns.selected_value_indices, row);
    }

    // 从数据库读取指定风洞试验品的窗口数据，执行推断并组织结果分段。
    static void process_outdb_inference_for_dataset(struct xdb_ref* ref,
                                                    const LoadedDataset& ds,
                                                    const ExperimentConfig& cfg,
                                                    const QueryRange& qr,
                                                    TeeLogger& log,
                                                    double& read_seconds,
                                                    double& infer_seconds,
                                                    double& merge_seconds,
                                                    long long& before_count,
                                                    long long& after_count) {
        std::vector<size_t> sampled_indices;
        sampled_indices.reserve(ds.timestamps.size());

        int seen_in_range = 0;
        for (size_t i = 0; i < ds.timestamps.size(); ++i) {
            if (!in_range(ds.timestamps[i].value, qr)) continue;
            if ((seen_in_range % cfg.sampling_step) == 0) sampled_indices.push_back(i);
            ++seen_in_range;
        }

        std::vector<Segment> merged_segments;
        const int T = cfg.window_length;
        const size_t window_count = sampled_indices.size() / static_cast<size_t>(T);

        for (size_t w = 0; w < window_count; ++w) {
            std::vector<std::vector<double>> window_data;
            window_data.reserve(static_cast<size_t>(T));

            bool ok = true;
            std::string start_label;
            std::string end_label;

            for (int j = 0; j < T; ++j) {
                const size_t idx = sampled_indices[w * static_cast<size_t>(T) + static_cast<size_t>(j)];
                const TimestampRef& ts = ds.timestamps[idx];

                if (j == 0) start_label = ts.label;
                if (j == T - 1) end_label = ts.label;

                std::vector<double> row;
                {
                    ScopedAccumulator acc(&read_seconds);
                    ok = read_one_row_from_db(ref, ds, ts, row);
                }

                if (!ok) break;
                window_data.push_back(std::move(row));
            }

            if (!ok || static_cast<int>(window_data.size()) != T) {
                log.line("警告：读取窗口失败，试验品=" + ds.spec.base_name + "，窗口起点=" + start_label);
                continue;
            }

            ++before_count;

            CDiTResult graph;
            {
                ScopedAccumulator acc(&infer_seconds);

                const std::vector<int>* src = ds.columns.source_columns.empty() ? nullptr : &ds.columns.source_columns;
                const std::vector<int>* tgt = ds.columns.target_columns.empty() ? nullptr : &ds.columns.target_columns;

                if (cfg.use_covmeta_cdit) {
                    graph = run_cdit_covmeta(window_data, cfg.tau, 0.01, false, src, tgt);
                } else {
                    graph = run_cdit_original(window_data, cfg.tau, 0.01, false, src, tgt);
                }
            }

            Segment current{start_label, end_label, graph};

            if (!merged_segments.empty()) {
                MergeCGResult mr;
                {
                    ScopedAccumulator acc(&merge_seconds);
                    mr = merge_adjacent_causal_graphs(
                            merged_segments.back().graph,
                            current.graph,
                            cfg.tau,
                            cfg.delta,
                            cfg.theta,
                            true);
                }

                if (mr.merge) {
                    merged_segments.back().end_label = current.end_label;
                    merged_segments.back().graph = std::move(mr.merged);
                } else {
                    merged_segments.push_back(std::move(current));
                }
            } else {
                merged_segments.push_back(std::move(current));
            }
        }

        after_count += static_cast<long long>(merged_segments.size());

        log.line("试验品：" + ds.spec.base_name);

        for (const Segment& seg : merged_segments) {
            log.line(seg.start_label + "到" + seg.end_label + " | 推断：" +
                     format_graph_ordered(seg.graph,
                                          ds.columns.all_aliases,
                                          ds.columns.cause_aliases,
                                          ds.columns.effect_aliases));
        }

        if (merged_segments.empty()) log.line("（无完整因果分析窗口可推断）");
    }

    static void append_stats_header(std::ofstream& out) {
        write_utf8_bom(out);
        out << "指定试验品集合,查询时间起止范围,参与的原因列,参与的结果列,采样步长,完整子随机过程长度T,推断时的τ,合并时的Δ,合并时的θ,MemTable容量(MB),是否使用协方差缓存驱动CDiT,总耗时(s),插入总耗时(s),合并压缩率,库外推断中仅读取部分耗时(s),库外推断中仅推断部分耗时(s),仅合并部分耗时(s)\n";
    }

    static void append_stats_row(std::ofstream& out,
                                 const ExperimentConfig& cfg,
                                 double total_seconds,
                                 double insert_seconds,
                                 double compression_ratio,
                                 double read_seconds,
                                 double infer_seconds,
                                 double merge_seconds) {
        out << csv_escape(cfg.sample_spec) << ','
            << csv_excel_text(cfg.range_text) << ','
            << csv_escape(cfg.cause_spec) << ','
            << csv_escape(cfg.effect_spec) << ','
            << cfg.sampling_step << ','
            << cfg.window_length << ','
            << cfg.tau << ','
            << cfg.delta << ','
            << format_theta(cfg.theta) << ','
            << cfg.memtable_mb << ','
            << bool_zh(cfg.use_covmeta_cdit) << ','
            << format_seconds(total_seconds) << ','
            << format_seconds(insert_seconds) << ','
            << format_percent(compression_ratio) << ','
            << format_seconds(read_seconds) << ','
            << format_seconds(infer_seconds) << ','
            << format_seconds(merge_seconds) << "\n";
        out.flush();
    }

    static bool open_db_for_write(const ExperimentConfig& cfg, struct xdb*& db, struct xdb_ref*& ref) {
        db = remixdb_open(kDbDir.c_str(), 256, static_cast<size_t>(cfg.memtable_mb), true);
        if (!db) return false;
        ref = remixdb_ref(db);
        return ref != nullptr;
    }

    static bool open_db_for_read(struct xdb*& db, struct xdb_ref*& ref, int memtable_mb) {
        db = remixdb_open(kDbDir.c_str(), 256, static_cast<size_t>(std::max(1, memtable_mb)), true);
        if (!db) return false;
        ref = remixdb_ref(db);
        return ref != nullptr;
    }

    static void close_db(struct xdb*& db, struct xdb_ref*& ref) {
        if (ref) {
            remixdb_unref(ref);
            ref = nullptr;
        }
        if (db) {
            remixdb_close(db);
            db = nullptr;
        }
    }

    // 执行单组 WindTunnel 入库与库外推断对照实验，并统计各处理阶段耗时。
    static int run_one_experiment(int exp_id,
                                  const ExperimentConfig& cfg,
                                  TeeLogger& log,
                                  std::ofstream& stats) {
        std::filesystem::remove_all(kDbDir);

        Timer total_timer;
        causal_meta_set_enabled(0);

        std::vector<DatasetSpec> specs = expand_sample_spec(cfg.sample_spec);
        std::vector<std::string> expanded_names;
        for (const DatasetSpec& s : specs) expanded_names.push_back(s.base_name);

        log.line("风洞实验第" + std::to_string(exp_id) + "次实验");
        log.line("本次数据库外纯CDiT推断+Merge实验参数设置：试验品=" + join_strings(expanded_names, ",") +
                 "；因果查询起止=" + cfg.range_text +
                 "；是否使用协方差缓存驱动CDiT：" + bool_zh(cfg.use_covmeta_cdit) + "；");
        log.line();

        std::vector<LoadedDataset> loaded;
        double insert_seconds = 0.0;
        {
            Timer insert_timer;
            struct xdb* db = nullptr;
            struct xdb_ref* ref = nullptr;
            if (!open_db_for_write(cfg, db, ref)) {
                log.line("错误：无法打开 RemixDB 写入实例");
                std::filesystem::remove_all(kDbDir);
                return 1;
            }
            for (const DatasetSpec& spec : specs) {
                LoadedDataset ds;
                if (insert_one_dataset(ref, spec, cfg, ds, log)) loaded.push_back(std::move(ds));
            }
            remixdb_sync(ref);
            close_db(db, ref);
            insert_seconds = insert_timer.seconds();
        }

        double read_seconds = 0.0;
        double infer_seconds = 0.0;
        double merge_seconds = 0.0;
        double range_seconds = 0.0;
        long long before_count = 0;
        long long after_count = 0;
        {
            Timer range_timer;
            struct xdb* db = nullptr;
            struct xdb_ref* ref = nullptr;
            if (!open_db_for_read(db, ref, cfg.memtable_mb)) {
                log.line("错误：无法重新打开 RemixDB 读取实例");
                std::filesystem::remove_all(kDbDir);
                return 1;
            }

            QueryRange qr = parse_query_range(cfg.range_text);
            log.line("本次范围推断输入：试验品=" + join_strings(expanded_names, ",") +
                     "；因果查询起止=" + cfg.range_text +
                     "；参与的原因列=" + cfg.cause_spec +
                     "；参与的结果列=" + cfg.effect_spec +
                     "；采样步长=" + std::to_string(cfg.sampling_step) +
                     "；T=" + std::to_string(cfg.window_length) +
                     "；τ=" + std::to_string(cfg.tau) +
                     "；Δ=" + std::to_string(cfg.delta) +
                     "；θ=" + format_theta(cfg.theta));

            for (const LoadedDataset& ds : loaded) {
                process_outdb_inference_for_dataset(ref,
                                                    ds,
                                                    cfg,
                                                    qr,
                                                    log,
                                                    read_seconds,
                                                    infer_seconds,
                                                    merge_seconds,
                                                    before_count,
                                                    after_count);
            }

            close_db(db, ref);
            range_seconds = range_timer.seconds();
        }

        const double compression = before_count > 0
                                   ? (static_cast<double>(before_count - after_count) / static_cast<double>(before_count)) * 100.0
                                   : 0.0;
        log.line("本次范围查询小节：耗时:" + format_seconds(range_seconds) +
                 "s，合并压缩率:" + format_percent(compression));
        log.line();

        const double total_seconds = total_timer.seconds();
        append_stats_row(stats,
                         cfg,
                         total_seconds,
                         insert_seconds,
                         compression,
                         read_seconds,
                         infer_seconds,
                         merge_seconds);

        std::filesystem::remove_all(kDbDir);
        return 0;
    }

} // 匿名命名空间结束

// WindTunnel 入库与库外推断对照实验入口，读取配置并逐项运行。
extern "C" int run_outdb_windtunnel_experiments(void) {
    std::filesystem::create_directories(kLogDir);
    TeeLogger log(kPrintLog);

    std::ofstream stats(kStatsCsv, std::ios::binary | std::ios::trunc);
    if (!stats.good()) {
        log.line("错误：无法创建统计日志：" + kStatsCsv);
        return 1;
    }
    append_stats_header(stats);

    std::vector<ExperimentConfig> configs = read_experiment_configs(log);
    if (configs.empty()) {
        log.line("没有可运行的风洞入库+库外推断实验配置。");
        return 1;
    }

    int rc = 0;
    for (size_t i = 0; i < configs.size(); ++i) {
        int one = run_one_experiment(static_cast<int>(i + 1), configs[i], log, stats);
        if (one != 0) rc = one;
    }
    return rc;
}
