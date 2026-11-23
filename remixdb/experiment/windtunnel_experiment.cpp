/*
 * 本文件实现 WindTunnel 数据集的实验流程，包括数据读取、窗口组织、数据库写入、查询、计时和结果日志输出。
 */

#include "windtunnel_experiment.h"

#include "../causal_meta.h"
#include "../xdb.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

    const std::string kDataDir = "data/windtunnel_data";
    const std::string kMetaDir = "file/windtunnel_experiment";
    const std::string kXdbDir = "xdb_windtunnel_experiment";
    const std::string kLogDir = "experiment_log";
    const std::string kInputFile = "experiment/windtunnel_experiment_input.csv";
    const std::string kSummaryFile = "experiment_log/风洞实验全部实验小结.csv";
    const std::string kPrintLogFile = "experiment_log/风洞实验全部实验打印日志.txt";

    struct InputRow {
        std::string trials_raw;
        std::string time_range_raw;
        std::string cause_cols_raw;
        std::string effect_cols_raw;
        int sampling_step = 1;
        int window_length = 250;
        int tau = 5;
        int delta = 2;
        double theta = 0.8;
        int memtable_mb = 2;
        bool test_fault = false;
        bool print_log = false;
        bool use_covariance_cache_cdit = false;
    };

    struct TimeRange {
        bool has_start = false;
        bool has_end = false;
        double start = 0.0;
        double end = 0.0;
        std::string display = "-∞到+∞";
    };

    struct CsvTable {
        std::vector<std::string> header;
        std::vector<std::vector<std::string>> rows;
    };

    struct TrialInfo {
        std::string short_id;
        std::string base_name;
        std::string display_name;
    };

    struct PreparedDataset {
        TrialInfo trial;
        std::string csv_path;
        std::vector<std::string> selected_aliases;
        std::vector<int> selected_indexes;
        std::vector<CausalMetaTimestampInfo> timestamps;
        std::vector<std::pair<std::string, std::string>> kvs;
    };

    struct Logger {
        std::ofstream file;
        bool console = true;

        void open_append(const std::string& path) {
            file.open(path, std::ios::binary | std::ios::app);
        }

        void write(const std::string& s) {
            if (console) {
                std::cout << s;
                std::cout.flush();
            }
            if (file.is_open()) {
                file << s;
                file.flush();
            }
        }

        void writeln(const std::string& s = std::string()) {
            write(s + "\n");
        }
    };

    static void causal_log_sink(const char* msg, void* userdata) {
        if (!userdata || !msg) return;
        static_cast<Logger*>(userdata)->write(std::string(msg));
    }

    static double wall_seconds() {
        using clock = std::chrono::steady_clock;
        static const auto t0 = clock::now();
        auto now = clock::now();
        return std::chrono::duration<double>(now - t0).count();
    }

    static std::string trim(const std::string& s) {
        size_t l = 0, r = s.size();
        while (l < r && std::isspace((unsigned char)s[l])) ++l;
        while (r > l && std::isspace((unsigned char)s[r - 1])) --r;
        return s.substr(l, r - l);
    }

    static bool starts_with(const std::string& s, const std::string& p) {
        return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin());
    }

    static bool ends_with(const std::string& s, const std::string& p) {
        return s.size() >= p.size() && std::equal(p.rbegin(), p.rend(), s.rbegin());
    }

    static std::vector<std::string> split_csv_line(const std::string& line) {
        std::vector<std::string> out;
        std::string cur;
        bool in_quotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '"') {
                if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    in_quotes = !in_quotes;
                }
            } else if (c == ',' && !in_quotes) {
                out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        out.push_back(cur);
        return out;
    }

    static void replace_all(std::string& s, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    static std::vector<std::string> split_list(const std::string& s) {
        std::string x = s;
        replace_all(x, "，", ",");
        replace_all(x, "、", ",");
        for (char& c : x) {
            if (c == ';' || c == '\t') c = ',';
        }
        std::vector<std::string> out;
        std::string cur;
        std::istringstream iss(x);
        while (std::getline(iss, cur, ',')) {
            std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
        }
        return out;
    }

    static std::vector<std::string> split_trials(const std::string& s) {
        std::string x = s;
        replace_all(x, "，", " ");
        replace_all(x, "、", " ");
        for (char& c : x) {
            if (c == ',' || c == ';' || c == '\t') c = ' ';
        }
        std::vector<std::string> out;
        std::istringstream iss(x);
        std::string t;
        while (iss >> t) out.push_back(trim(t));
        return out;
    }

    static std::string strip_excel_formula_wrapper(const std::string& raw) {
        std::string s = trim(raw);
        if (starts_with(s, "=\"") && ends_with(s, "\"")) {
            s = s.substr(2, s.size() - 3);
        }
        if (starts_with(s, "='") && ends_with(s, "'")) {
            s = s.substr(2, s.size() - 3);
        }
        return trim(s);
    }

    static bool yes_value(const std::string& raw) {
        std::string s = trim(raw);
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s == "是" || s == "yes" || s == "y" || s == "true" || s == "1";
    }

    static std::string yes_no(bool v) {
        return v ? "是" : "否";
    }

    static std::string format_double2(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string fixed_string(double x, int n) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(n) << x;
        return oss.str();
    }

    static std::string percent_string(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << x << "%";
        return oss.str();
    }

    static TimeRange parse_time_range(const std::string& raw) {
        std::string s = strip_excel_formula_wrapper(raw);
        TimeRange tr;
        tr.display = s.empty() ? "-∞到+∞" : s;
        if (s.empty() || s == "-∞到+∞" || s == "-inf到+inf" || s == "-INF到+INF") return tr;
        size_t pos = s.find("到");
        bool used_chinese_to = true;
        if (pos == std::string::npos) {
            used_chinese_to = false;
            pos = s.find('~');
            if (pos == std::string::npos) pos = s.find('-');
        }
        if (pos == std::string::npos) return tr;
        std::string a = trim(s.substr(0, pos));
        std::string b = trim(s.substr(pos + (used_chinese_to ? std::string("到").size() : 1)));
        if (!a.empty() && a != "-∞" && a != "-inf" && a != "-INF") {
            tr.has_start = true;
            tr.start = std::strtod(a.c_str(), nullptr);
        }
        if (!b.empty() && b != "+∞" && b != "+inf" && b != "+INF") {
            tr.has_end = true;
            tr.end = std::strtod(b.c_str(), nullptr);
        }
        return tr;
    }

    static std::string csv_escape(const std::string& raw, bool excel_formula_for_leading_minus = false) {
        std::string s = raw;
        if (excel_formula_for_leading_minus && !s.empty() && s[0] == '-') {
            s = "=\"" + s + "\"";
        }
        bool need_quote = s.find(',') != std::string::npos || s.find('"') != std::string::npos ||
                          s.find('\n') != std::string::npos || s.find('\r') != std::string::npos ||
                          (!s.empty() && (s[0] == '=' || s[0] == '+'));
        if (!need_quote) return s;
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\"\"";
            else out.push_back(c);
        }
        out.push_back('"');
        return out;
    }

    static std::string join(const std::vector<std::string>& xs, const std::string& delim) {
        std::ostringstream oss;
        for (size_t i = 0; i < xs.size(); ++i) {
            if (i) oss << delim;
            oss << xs[i];
        }
        return oss.str();
    }

    static std::map<std::string, TrialInfo> build_trial_map() {
        std::map<std::string, TrialInfo> m;
        for (int i = 1; i <= 16; ++i) {
            std::string sid = "w" + std::to_string(i);
            std::string base = "actuators_random_walk_" + std::to_string(i);
            m[sid] = {sid, base, base};
        }
        m["w"] = {"w", "actuators_random_walk", "actuators_random_walk"};
        for (int i = 1; i <= 5; ++i) {
            std::string sid = "f" + std::to_string(i);
            std::string base = "loads_hatch_mix_fast_run_" + std::to_string(i);
            m[sid] = {sid, base, base};
        }
        m["f"] = {"f", "loads_hatch_mix_fast_run", "loads_hatch_mix_fast_run"};
        for (int i = 1; i <= 5; ++i) {
            std::string sid = "s" + std::to_string(i);
            std::string base = "loads_hatch_mix_slow_run_" + std::to_string(i);
            m[sid] = {sid, base, base};
        }
        m["s"] = {"s", "loads_hatch_mix_slow_run", "loads_hatch_mix_slow_run"};
        m["mul"] = {"mul", "regime_jumps_multi", "regime_jumps_multi"};
        m["sin"] = {"sin", "regime_jumps_single", "regime_jumps_single"};
        return m;
    }

    static std::map<std::string, std::string> alias_to_header_map() {
        return {
                {"H", "hatch"}, {"A1", "pot_1"}, {"A2", "pot_2"},
                {"Lin", "load_in"}, {"Lout", "load_out"},
                {"R1", "v_1"}, {"R2", "v_2"}, {"RM", "v_mic"}, {"Rin", "v_in"}, {"Rout", "v_out"},
                {"Cin", "current_in"}, {"Cout", "current_out"},
                {"ωin", "rpm_in"}, {"win", "rpm_in"}, {"omega_in", "rpm_in"},
                {"ωout", "rpm_out"}, {"wout", "rpm_out"}, {"omega_out", "rpm_out"},
                {"Pup", "pressure_upwind"}, {"Pdw", "pressure_downwind"}, {"Pamb", "pressure_ambient"}, {"Pint", "pressure_intake"},
                {"M", "mic"}, {"S1", "signal_1"}, {"S2", "signal_2"}
        };
    }

    static std::string canonical_alias(const std::string& raw) {
        std::string s = trim(raw);
        if (s == "win" || s == "omega_in") return "ωin";
        if (s == "wout" || s == "omega_out") return "ωout";
        return s;
    }

    static std::string csv_path_for(const TrialInfo& trial, const std::string& fault_suffix) {
        std::string path = kDataDir + "/" + trial.base_name + "_pre";
        if (!fault_suffix.empty()) path += "_" + fault_suffix;
        path += ".csv";
        return path;
    }

    static std::vector<std::string> discover_fault_suffixes(const std::vector<TrialInfo>& trials) {
        std::vector<std::string> out;
        out.push_back("");
        if (trials.empty() || !std::filesystem::exists(kDataDir)) return out;
        std::set<std::string> candidate;
        for (const auto& entry : std::filesystem::directory_iterator(kDataDir)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            for (const TrialInfo& t : trials) {
                std::string prefix = t.base_name + "_pre_";
                if (starts_with(name, prefix) && ends_with(name, ".csv")) {
                    candidate.insert(name.substr(prefix.size(), name.size() - prefix.size() - 4));
                }
            }
        }
        std::vector<std::string> kept;
        for (const std::string& suffix : candidate) {
            bool all_exist = true;
            for (const TrialInfo& t : trials) {
                if (!std::filesystem::exists(csv_path_for(t, suffix))) {
                    all_exist = false;
                    break;
                }
            }
            if (all_exist) kept.push_back(suffix);
        }
        auto rate = [](const std::string& s) -> int {
            size_t p = s.find('%');
            if (p == std::string::npos) return 100000;
            return std::atoi(s.substr(0, p).c_str());
        };
        std::sort(kept.begin(), kept.end(), [&](const std::string& a, const std::string& b) {
            int ra = rate(a), rb = rate(b);
            if (ra != rb) return ra < rb;
            return a < b;
        });
        out.insert(out.end(), kept.begin(), kept.end());
        return out;
    }

    static CsvTable read_csv_table(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        CsvTable t;
        if (!in.good()) return t;
        std::string line;
        if (!std::getline(in, line)) return t;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
            line = line.substr(3);
        }
        t.header = split_csv_line(line);
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (trim(line).empty()) continue;
            t.rows.push_back(split_csv_line(line));
        }
        return t;
    }

    static std::unordered_map<std::string, int> header_index(const std::vector<std::string>& header) {
        std::unordered_map<std::string, int> m;
        for (size_t i = 0; i < header.size(); ++i) m[trim(header[i])] = (int)i;
        return m;
    }

    static std::string selected_value_string(const std::vector<std::string>& row, const std::vector<int>& idxs) {
        std::ostringstream oss;
        for (size_t i = 0; i < idxs.size(); ++i) {
            if (i) oss << ',';
            int idx = idxs[i];
            if (idx >= 0 && idx < (int)row.size()) oss << trim(row[(size_t)idx]);
            else oss << '0';
        }
        return oss.str();
    }

    // 加载指定风洞试验及故障版本，完成列选择、时间戳整理和窗口输入准备。
    static PreparedDataset prepare_dataset(const TrialInfo& trial,
                                           const std::string& csv_path,
                                           const std::vector<std::string>& cause_aliases,
                                           const std::vector<std::string>& effect_aliases,
                                           const TimeRange& range,
                                           Logger& logger) {
        PreparedDataset pd;
        pd.trial = trial;
        pd.csv_path = csv_path;

        CsvTable table = read_csv_table(csv_path);
        if (table.header.empty()) {
            logger.writeln("警告：无法读取数据集文件：" + csv_path);
            return pd;
        }
        auto hidx = header_index(table.header);
        auto amap = alias_to_header_map();

        std::vector<std::string> selected_aliases;
        std::set<std::string> seen_alias;
        auto add_alias = [&](const std::string& alias0) {
            std::string alias = canonical_alias(alias0);
            if (alias.empty() || seen_alias.count(alias)) return;
            auto ait = amap.find(alias);
            if (ait == amap.end() || hidx.find(ait->second) == hidx.end()) {
                logger.writeln("警告：列 " + alias + " 不存在于 " + csv_path + "，已忽略。 ");
                return;
            }
            seen_alias.insert(alias);
            selected_aliases.push_back(alias);
            pd.selected_indexes.push_back(hidx[ait->second]);
        };
        for (const auto& a : cause_aliases) add_alias(a);
        for (const auto& a : effect_aliases) add_alias(a);
        pd.selected_aliases = selected_aliases;
        if (pd.selected_aliases.empty()) {
            logger.writeln("警告：没有可插入列，跳过数据集：" + csv_path);
            return pd;
        }

        int ts_idx = hidx.count("timestamp") ? hidx["timestamp"] : 0;
        std::set<std::string> registered;
        for (const auto& row : table.rows) {
            if (ts_idx >= (int)row.size()) continue;
            std::string ts_key = trim(row[(size_t)ts_idx]);
            if (ts_key.empty()) continue;
            double ts = std::strtod(ts_key.c_str(), nullptr);
            if (range.has_start && ts < range.start) continue;
            if (range.has_end && ts > range.end) continue;
            if (!registered.count(ts_key)) {
                registered.insert(ts_key);
                pd.timestamps.push_back({ts_key, format_double2(ts)});
            }
            std::string key = trial.short_id + "," + ts_key;
            std::string val = selected_value_string(row, pd.selected_indexes);
            pd.kvs.push_back(std::make_pair(std::move(key), std::move(val)));
        }
        return pd;
    }

    static void maybe_create_default_input() {
        if (std::filesystem::exists(kInputFile)) return;
        std::filesystem::create_directories("experiment");
        std::ofstream out(kInputFile, std::ios::binary | std::ios::trunc);
        out << u8"\xEF\xBB\xBF";
        out << "指定试验品集合,查询时间起止范围,参与的原因列,参与的结果列,采样步长,完整子随机过程长度T,推断时的τ,合并时的Δ,合并时的θ,MemTable容量(MB),是否测试故障数据集,是否打印过程日志,是否使用协方差缓存驱动CDiT\n";
        out << "w1 w16,\"=\"\"-∞到+∞\"\"\",\"Lin,Lout\",\"ωin,ωout,Pdw,Pint\",1,250,5,2,0.8,2,是,否,是\n";
    }

    // 读取 WindTunnel 实验输入 CSV，并将每行参数解析为一条试验配置。
    static std::vector<InputRow> read_input_rows(Logger& logger) {
        maybe_create_default_input();
        std::ifstream in(kInputFile, std::ios::binary);
        std::vector<InputRow> rows;
        if (!in.good()) {
            logger.writeln("错误：找不到实验输入文件：" + kInputFile);
            return rows;
        }
        std::string line;
        if (!std::getline(in, line)) return rows;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (trim(line).empty()) continue;
            auto f = split_csv_line(line);
            if (f.size() < 11) continue;
            InputRow r;
            r.trials_raw = trim(f[0]);
            r.time_range_raw = trim(f[1]);
            r.cause_cols_raw = trim(f[2]);
            r.effect_cols_raw = trim(f[3]);
            r.sampling_step = std::max(1, std::atoi(f[4].c_str()));
            r.window_length = std::max(1, std::atoi(f[5].c_str()));
            r.tau = std::max(1, std::atoi(f[6].c_str()));
            r.delta = std::max(1, std::atoi(f[7].c_str()));
            r.theta = std::strtod(f[8].c_str(), nullptr);
            r.memtable_mb = std::max(1, std::atoi(f[9].c_str()));
            r.test_fault = yes_value(f[10]);
            r.print_log = (f.size() >= 12) ? yes_value(f[11]) : false;
            r.use_covariance_cache_cdit = (f.size() >= 13) ? yes_value(f[12]) : false;
            rows.push_back(r);
        }
        return rows;
    }

    static std::vector<int> indexes_of(const std::vector<std::string>& selected_aliases, const std::vector<std::string>& wanted_aliases) {
        std::vector<int> out;
        for (const auto& w0 : wanted_aliases) {
            std::string w = canonical_alias(w0);
            for (size_t i = 0; i < selected_aliases.size(); ++i) {
                if (selected_aliases[i] == w) {
                    out.push_back((int)i + 1);
                    break;
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    static std::string format_graph_for_query(const CDiTResult& g,
                                              const std::vector<std::string>& all_names,
                                              const std::vector<std::string>& preferred_order) {
        std::map<std::string, int> rank;
        for (size_t i = 0; i < preferred_order.size(); ++i) rank[preferred_order[i]] = (int)i;
        auto name_of = [&](int idx) -> std::string {
            if (idx >= 1 && idx <= (int)all_names.size()) return all_names[(size_t)idx - 1];
            std::ostringstream oss;
            oss << 'X' << idx;
            return oss.str();
        };
        struct Item { int rp, rq, p, q; std::string ps, qs; std::vector<int> lags; };
        std::vector<Item> items;
        for (const auto& kv : g.lags_by_edge) {
            std::string ps = name_of(kv.first.first);
            std::string qs = name_of(kv.first.second);
            int rp = rank.count(ps) ? rank[ps] : 100000 + kv.first.first;
            int rq = rank.count(qs) ? rank[qs] : 100000 + kv.first.second;
            items.push_back({rp, rq, kv.first.first, kv.first.second, ps, qs, kv.second});
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.rp != b.rp) return a.rp < b.rp;
            if (a.rq != b.rq) return a.rq < b.rq;
            if (a.p != b.p) return a.p < b.p;
            return a.q < b.q;
        });
        std::ostringstream oss;
        for (const auto& item : items) {
            if (item.lags.empty()) continue;
            std::vector<int> lags = item.lags;
            std::sort(lags.begin(), lags.end());
            oss << item.ps << "→" << item.qs << "滞后=";
            for (int lag : lags) oss << lag << ',';
            oss << "; ";
        }
        return oss.str();
    }

    static std::vector<std::string> read_vars_from_metadata_file(const std::string& dataset_id) {
        std::ifstream in(kMetaDir + "/" + dataset_id + "_C.tsv", std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (starts_with(line, "#vars\t")) {
                std::vector<std::string> out;
                std::string rest = line.substr(6);
                std::string cur;
                std::istringstream iss(rest);
                while (std::getline(iss, cur, ',')) out.push_back(trim(cur));
                return out;
            }
        }
        return {};
    }

    // 针对一个风洞试验执行范围查询，汇总各窗口因果图和查询耗时。
    static void run_query_for_dataset(const TrialInfo& trial,
                                      const std::vector<std::string>& query_columns,
                                      double& total_query_seconds,
                                      Logger& logger) {
        double q0 = wall_seconds();
        auto segments = causal_meta_query_dataset_from_file(kMetaDir, trial.short_id, "-∞", "+∞", query_columns);
        std::vector<std::string> all_vars = read_vars_from_metadata_file(trial.short_id);
        double q1 = wall_seconds();
        total_query_seconds += q1 - q0;

        logger.writeln("本次范围查询输入：查询的试验品: " + trial.display_name + "；查询时间区间:-∞到+∞；查询列:" + join(query_columns, ","));
        logger.writeln("查询结果如下");
        for (const auto& seg : segments) {
            logger.writeln(seg.start_label + "到" + seg.end_label + " | 推断：" + format_graph_for_query(seg.graph, all_vars, query_columns));
        }
        logger.writeln("本次范围查询小节：耗时:" + fixed_string(q1 - q0, 4) + "s");
        logger.writeln();
    }

    static void write_summary_header() {
        std::filesystem::create_directories(kLogDir);
        std::ofstream out(kSummaryFile, std::ios::binary | std::ios::trunc);
        out << u8"\xEF\xBB\xBF";
        out << "插入的时间范围,插入的试验品集合,参与插入的原因列,参与插入的结果列,采样步长,插入时的T,推断时的τ,合并时的Δ,合并时的θ,MemTable(MB),是否测试故障数据集,是否打印过程日志,是否使用协方差缓存驱动CDiT,插入耗时(s),合并压缩率,仅合并部分耗时(s),全数据集全跨度全列范围查询总耗时(s),库内推断算法部分耗时(s),故障程度,全部页面数,互补页面数,重叠页面数,插入总耗时减库内推断耗时(s)\n";
    }

    static void append_summary_row(const InputRow& row,
                                   const std::string& actual_trials,
                                   const std::string& fault_label,
                                   double insert_seconds,
                                   double mcr,
                                   double query_seconds,
                                   const CausalMetaCounters& counters) {
        std::ofstream out(kSummaryFile, std::ios::binary | std::ios::app);
        std::vector<std::string> cells;
        cells.push_back(strip_excel_formula_wrapper(row.time_range_raw));
        cells.push_back(actual_trials);
        cells.push_back(join(split_list(row.cause_cols_raw), ","));
        cells.push_back(join(split_list(row.effect_cols_raw), ","));
        cells.push_back(std::to_string(row.sampling_step));
        cells.push_back(std::to_string(row.window_length));
        cells.push_back(std::to_string(row.tau));
        cells.push_back(std::to_string(row.delta));
        cells.push_back(fixed_string(row.theta, 2));
        cells.push_back(std::to_string(row.memtable_mb));
        cells.push_back(yes_no(row.test_fault));
        cells.push_back(yes_no(row.print_log));
        cells.push_back(yes_no(row.use_covariance_cache_cdit));
        cells.push_back(fixed_string(insert_seconds, 4));
        cells.push_back(percent_string(mcr));
        cells.push_back(fixed_string(counters.merge_seconds, 4));
        cells.push_back(fixed_string(query_seconds, 4));
        cells.push_back(fixed_string(counters.metadata_seconds, 4));
        cells.push_back(fault_label.empty() ? "无" : fault_label);
        cells.push_back(std::to_string(counters.total_pages));
        cells.push_back(std::to_string(counters.complementary_pages));
        cells.push_back(std::to_string(counters.overlapped_pages));
        cells.push_back(fixed_string(insert_seconds - counters.metadata_seconds, 4));

        for (size_t i = 0; i < cells.size(); ++i) {
            if (i) out << ',';
            out << csv_escape(cells[i], i == 0);
        }
        out << "\n";
    }

    // 执行一条 WindTunnel 配置，完成数据入库、元数据维护、查询和实验结果输出。
    static int run_one_experiment(const InputRow& row,
                                  int exp_no,
                                  const std::string& fault_suffix,
                                  const std::vector<TrialInfo>& trials,
                                  Logger& logger) {
        std::filesystem::remove_all(kMetaDir);
        std::filesystem::remove_all(kXdbDir);
        std::filesystem::create_directories(kMetaDir);
        std::filesystem::create_directories(kXdbDir);

        TimeRange range = parse_time_range(row.time_range_raw);
        std::vector<std::string> cause_aliases_raw = split_list(row.cause_cols_raw);
        std::vector<std::string> effect_aliases_raw = split_list(row.effect_cols_raw);
        std::vector<std::string> cause_aliases, effect_aliases;
        for (auto& x : cause_aliases_raw) cause_aliases.push_back(canonical_alias(x));
        for (auto& x : effect_aliases_raw) effect_aliases.push_back(canonical_alias(x));
        std::vector<std::string> query_columns = cause_aliases;
        for (const auto& e : effect_aliases) if (std::find(query_columns.begin(), query_columns.end(), e) == query_columns.end()) query_columns.push_back(e);

        std::vector<std::string> trial_ids;
        for (const auto& t : trials) trial_ids.push_back(t.short_id);

        logger.writeln("第" + std::to_string(exp_no) + "次实验");
        logger.writeln("数据库参数设置：MemTable=" + std::to_string(row.memtable_mb) + "MB");
        logger.writeln("本此插入实验参数设置：要插入的试验品=" + join(trial_ids, ",") +
                       "；插入时间范围=" + range.display +
                       "；原因列=" + join(cause_aliases, ",") +
                       "；结果列=" + join(effect_aliases, ",") +
                       "；采样步长=" + std::to_string(row.sampling_step) +
                       "；T=" + std::to_string(row.window_length) +
                       "；τ=" + std::to_string(row.tau) +
                       "；Δ=" + std::to_string(row.delta) +
                       "；θ=" + fixed_string(row.theta, 2) +
                       "；是否打印过程日志=" + yes_no(row.print_log) +
                       "；是否使用协方差缓存驱动CDiT=" + yes_no(row.use_covariance_cache_cdit) +
                       (fault_suffix.empty() ? "；故障程度=无" : "；故障程度=" + fault_suffix));
        logger.writeln();

        std::vector<PreparedDataset> datasets;
        std::vector<TrialInfo> actual_trials;
        for (const TrialInfo& trial : trials) {
            std::string path = csv_path_for(trial, fault_suffix);
            if (!std::filesystem::exists(path)) {
                logger.writeln("警告：找不到数据集文件，跳过：" + path);
                continue;
            }
            PreparedDataset pd = prepare_dataset(trial, path, cause_aliases, effect_aliases, range, logger);
            if (!pd.kvs.empty() && !pd.timestamps.empty()) {
                datasets.push_back(std::move(pd));
                actual_trials.push_back(trial);
            } else {
                logger.writeln("警告：数据集在指定时间范围内无可插入数据，跳过：" + path);
            }
        }

        if (datasets.empty()) {
            logger.writeln("本次实验没有可插入数据，已跳过。\n");
            return 0;
        }

        causal_meta_reset_all();
        causal_meta_set_enabled_cpp(true);
        causal_meta_set_metadata_dir(kMetaDir);
        causal_meta_set_page_logging(row.print_log);
        causal_meta_set_log_sink(causal_log_sink, &logger);

        for (const PreparedDataset& pd : datasets) {
            CausalMetaDatasetConfig cfg;
            cfg.dataset_id = pd.trial.short_id;
            cfg.display_name = pd.trial.display_name;
            cfg.variable_names = pd.selected_aliases;
            cfg.ordered_timestamps = pd.timestamps;
            cfg.sampling_step = row.sampling_step;
            cfg.window_length = row.window_length;
            cfg.tau = row.tau;
            cfg.delta = row.delta;
            cfg.theta = row.theta;
            cfg.alpha = 0.01;
            cfg.infer_self = false;
            cfg.ignore_self_in_merge = true;
            cfg.use_covariance_cache_cdit = row.use_covariance_cache_cdit;
            cfg.source_columns = indexes_of(pd.selected_aliases, cause_aliases);
            cfg.target_columns = indexes_of(pd.selected_aliases, effect_aliases);
            cfg.metadata_dir = kMetaDir;
            causal_meta_register_dataset(cfg);
        }

        double t0 = wall_seconds();
        struct xdb* db = remixdb_open(kXdbDir.c_str(), 256, (size_t)row.memtable_mb, true);
        if (!db) {
            logger.writeln("错误：RemixDB 打开失败。\n");
            causal_meta_clear_log_sink();
            return 1;
        }
        struct xdb_ref* ref = remixdb_ref(db);

        for (const PreparedDataset& pd : datasets) {
            std::set<std::string> seen_ts_in_this_file;
            bool in_repair_segment = false;
            for (const auto& kv : pd.kvs) {
                size_t comma = kv.first.find(',');
                std::string ts = comma == std::string::npos ? kv.first : kv.first.substr(comma + 1);
                bool duplicate_ts = !seen_ts_in_this_file.insert(ts).second;
                if (duplicate_ts && !in_repair_segment) {
                    remixdb_sync(ref);
                    in_repair_segment = true;
                }
                if (!duplicate_ts) in_repair_segment = false;
                remixdb_put(ref, kv.first.data(), (u32)kv.first.size(), kv.second.data(), (u32)kv.second.size());
            }
        }

        remixdb_causal_meta_flush(db);
        double t1 = wall_seconds();
        double insert_seconds = t1 - t0;
        CausalMetaCounters counters = causal_meta_get_counters();
        double mcr = 0.0;
        if (counters.original_complete_windows > 0) {
            mcr = ((double)counters.original_complete_windows - (double)counters.final_metadata_entries) * 100.0 /
                  (double)counters.original_complete_windows;
        }
        logger.writeln("本此插入完毕 | 耗时：" + fixed_string(insert_seconds, 4) + "s；合并压缩率：" + percent_string(mcr));
        logger.writeln();

        double total_query_seconds = 0.0;
        for (const TrialInfo& trial : actual_trials) {
            run_query_for_dataset(trial, query_columns, total_query_seconds, logger);
        }

        remixdb_unref(ref);
        remixdb_close(db);
        causal_meta_clear_log_sink();

        std::vector<std::string> actual_ids;
        for (const auto& t : actual_trials) actual_ids.push_back(t.short_id);
        append_summary_row(row, join(actual_ids, ","), fault_suffix.empty() ? "无" : fault_suffix,
                           insert_seconds, mcr, total_query_seconds, counters);

        std::filesystem::remove_all(kXdbDir);
        return 0;
    }

} // 匿名命名空间结束

// WindTunnel 实验总入口：读取配置并依次执行各试验品及其故障版本。
extern "C" int run_windtunnel_experiments(void) {
    try {
        std::filesystem::create_directories(kLogDir);
        std::filesystem::create_directories(kDataDir);
        std::filesystem::create_directories(kMetaDir);
        write_summary_header();
        {
            std::ofstream clear(kPrintLogFile, std::ios::binary | std::ios::trunc);
            clear << u8"\xEF\xBB\xBF";
        }

        Logger logger;
        logger.open_append(kPrintLogFile);

        auto input_rows = read_input_rows(logger);
        auto trial_map = build_trial_map();
        int exp_no = 0;
        for (const InputRow& row : input_rows) {
            std::vector<TrialInfo> trials;
            for (const std::string& sid0 : split_trials(row.trials_raw)) {
                std::string sid = trim(sid0);
                auto it = trial_map.find(sid);
                if (it != trial_map.end()) trials.push_back(it->second);
                else logger.writeln("警告：未知试验品缩写，已忽略：" + sid);
            }
            if (trials.empty()) {
                logger.writeln("警告：该行没有有效试验品，已跳过：" + row.trials_raw);
                continue;
            }

            std::vector<std::string> suffixes;
            if (row.test_fault) suffixes = discover_fault_suffixes(trials);
            else suffixes = {""};
            if (suffixes.empty()) suffixes = {""};
            for (const std::string& suffix : suffixes) {
                ++exp_no;
                int rc = run_one_experiment(row, exp_no, suffix, trials, logger);
                if (rc != 0) return rc;
            }
        }
        logger.writeln("全部风洞实验完成。小结文件：" + kSummaryFile);
        logger.writeln("全部风洞实验打印日志：" + kPrintLogFile);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "风洞实验异常：" << e.what() << std::endl;
        return 1;
    }
}
