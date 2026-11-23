/*
 * 本文件实现 Basin 数据集的库外处理对照实验，负责数据加载、窗口分析、结果组织和耗时统计。
 */

#include "outdb_basin_experiment.h"

#include "../xdb.h"
#include "../cdit_module.h"
#include "../merge_cg.h"
#include "../causal_meta.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

    using Clock = std::chrono::steady_clock;

    struct BasinRow {
        std::string timestamp;
        int date_key = 0;
        std::vector<double> values;
    };

    struct BasinDataset {
        std::string id;
        std::string path;
        std::vector<BasinRow> rows_in_range;
        std::vector<const BasinRow*> sampled_rows;
    };

    struct ExperimentConfig {
        std::string raw_sample_selector;
        std::string raw_time_range;
        int sampling_step = 1;
        int window_length = 365;
        int tau = 5;
        int delta = 2;
        double theta = 0.8;
        int memtable_mb = 2;
        bool use_covmeta_cdit = false;
    };

    struct GraphSegment {
        std::string start_label;
        std::string end_label;
        CDiTResult graph;
    };

    struct RemixSession {
        struct xdb* db = nullptr;
        struct xdb_ref* ref = nullptr;

        RemixSession(const std::string& dir, int memtable_mb) {
            db = remixdb_open(dir.c_str(), 256, (size_t)memtable_mb, true);
            if (!db) throw std::runtime_error("remixdb_open失败: " + dir);
            ref = remixdb_ref(db);
            if (!ref) throw std::runtime_error("remixdb_ref失败");
        }

        RemixSession(const RemixSession&) = delete;
        RemixSession& operator=(const RemixSession&) = delete;

        ~RemixSession() {
            close();
        }

        void sync() {
            if (ref) remixdb_sync(ref);
        }

        void close() {
            if (ref) {
                remixdb_unref(ref);
                ref = nullptr;
            }
            if (db) {
                remixdb_close(db);
                db = nullptr;
            }
        }
    };

    struct TeeLogger {
        std::ofstream file;

        explicit TeeLogger(const std::string& path) {
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            file.open(path, std::ios::binary | std::ios::trunc);
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

    static double seconds_since(const Clock::time_point& t0) {
        return std::chrono::duration<double>(Clock::now() - t0).count();
    }

    static std::string trim(const std::string& s) {
        size_t l = 0;
        size_t r = s.size();
        while (l < r && std::isspace((unsigned char)s[l])) ++l;
        while (r > l && std::isspace((unsigned char)s[r - 1])) --r;
        return s.substr(l, r - l);
    }

    static bool ends_with(const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    static std::string strip_excel_wrappers(std::string s) {
        s = trim(s);
        bool changed = true;
        while (changed) {
            changed = false;
            s = trim(s);
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                s = s.substr(1, s.size() - 2);
                changed = true;
            }
            s = trim(s);
            if (!s.empty() && s.front() == '=') {
                s.erase(s.begin());
                changed = true;
            }
            s = trim(s);
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                s = s.substr(1, s.size() - 2);
                changed = true;
            }
        }
        return trim(s);
    }

    static std::string lowercase_ascii(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    static bool parse_yes_no_field(const std::string& raw, bool default_value) {
        std::string s = lowercase_ascii(strip_excel_wrappers(raw));
        s = trim(s);
        if (s.empty()) return default_value;
        if (s == "是" || s == "1" || s == "true" || s == "yes" || s == "y" || s == "on" || s == "启用" || s == "使用") return true;
        if (s == "否" || s == "0" || s == "false" || s == "no" || s == "n" || s == "off" || s == "禁用" || s == "不使用") return false;
        if (s.find("是") != std::string::npos || s.find("true") != std::string::npos || s.find("yes") != std::string::npos) return true;
        if (s.find("否") != std::string::npos || s.find("false") != std::string::npos || s.find("no") != std::string::npos) return false;
        return default_value;
    }

    static std::string yes_no_text(bool v) {
        return v ? "是" : "否";
    }

    static std::vector<std::string> parse_csv_line(const std::string& line) {
        std::vector<std::string> fields;
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
                fields.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        fields.push_back(cur);
        return fields;
    }

    static std::vector<std::string> split_ws(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream iss(s);
        std::string x;
        while (iss >> x) out.push_back(x);
        return out;
    }

    static bool is_integer_string(const std::string& s) {
        std::string t = trim(s);
        if (t.empty()) return false;
        for (char c : t) {
            if (!std::isdigit((unsigned char)c)) return false;
        }
        return true;
    }

    static int parse_date_key(const std::string& raw) {
        std::string s = strip_excel_wrappers(raw);
        for (char& c : s) {
            if (c == '/') c = '-';
        }
        int y = 0, m = 0, d = 0;
        char dash1 = 0, dash2 = 0;
        std::istringstream iss(s);
        if (!(iss >> y >> dash1 >> m >> dash2 >> d) || dash1 != '-' || dash2 != '-' || y <= 0 || m <= 0 || d <= 0) {
            return 0;
        }
        return y * 10000 + m * 100 + d;
    }

    struct TimeRange {
        bool start_inf = true;
        bool end_inf = true;
        int start_key = 0;
        int end_key = 0;
        std::string display = "-∞到+∞";
    };

    static TimeRange parse_time_range(const std::string& raw) {
        TimeRange tr;
        std::string s = strip_excel_wrappers(raw);
        if (s.empty()) s = "-∞到+∞";
        tr.display = s;
        size_t pos = s.find("到");
        if (pos == std::string::npos) {
            // 回退处理：若只解析到一个有效日期，则将其同时作为起始日期和结束日期。
            int key = parse_date_key(s);
            if (key > 0) {
                tr.start_inf = false;
                tr.end_inf = false;
                tr.start_key = key;
                tr.end_key = key;
            }
            return tr;
        }
        std::string a = trim(s.substr(0, pos));
        std::string b = trim(s.substr(pos + std::strlen("到")));
        tr.start_inf = (a.empty() || a.find("-∞") != std::string::npos || a.find("-inf") != std::string::npos || a.find("-INF") != std::string::npos);
        tr.end_inf = (b.empty() || b.find("+∞") != std::string::npos || b.find("inf") != std::string::npos || b.find("INF") != std::string::npos);
        if (!tr.start_inf) tr.start_key = parse_date_key(a);
        if (!tr.end_inf) tr.end_key = parse_date_key(b);
        return tr;
    }

    static bool date_in_range(int key, const TimeRange& tr) {
        if (key <= 0) return false;
        if (!tr.start_inf && key < tr.start_key) return false;
        if (!tr.end_inf && key > tr.end_key) return false;
        return true;
    }

    static std::string csv_escape(const std::string& s) {
        bool need_quote = s.find(',') != std::string::npos || s.find('"') != std::string::npos ||
                          s.find('\n') != std::string::npos || s.find('\r') != std::string::npos;
        std::string out;
        if (need_quote) out.push_back('"');
        for (char c : s) {
            if (c == '"') out += "\"\"";
            else out.push_back(c);
        }
        if (need_quote) out.push_back('"');
        return out;
    }

    static std::string excel_formula_text_field(const std::string& s) {
        std::string escaped;
        for (char c : s) {
            if (c == '"') escaped += "\"\"";
            else escaped.push_back(c);
        }
        return "\"=\"\"" + escaped + "\"\"\"";
    }

    static std::string format_double(double x, int precision = 4) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(precision) << x;
        return oss.str();
    }

    static std::string format_compact_double(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << x;
        std::string s = oss.str();
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        if (s.empty()) s = "0";
        return s;
    }

    static std::string format_percent(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << x << "%";
        return oss.str();
    }

    static std::string value_to_string(const std::vector<double>& values) {
        std::ostringstream oss;
        oss << std::setprecision(17);
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) oss << ',';
            oss << values[i];
        }
        return oss.str();
    }

    static std::vector<double> parse_value_string(const std::string& s) {
        std::vector<double> out;
        std::string cur;
        std::istringstream iss(s);
        while (std::getline(iss, cur, ',')) {
            cur = trim(cur);
            if (cur.empty()) out.push_back(0.0);
            else out.push_back(std::strtod(cur.c_str(), nullptr));
        }
        return out;
    }

    static std::vector<std::string> list_basin_ids(const std::string& data_dir) {
        std::vector<std::string> ids;
        if (!std::filesystem::exists(data_dir)) return ids;
        for (const auto& ent : std::filesystem::directory_iterator(data_dir)) {
            if (!ent.is_regular_file()) continue;
            std::string name = ent.path().filename().string();
            if (!ends_with(name, "_pre.csv")) continue;
            ids.push_back(name.substr(0, name.size() - std::strlen("_pre.csv")));
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        return ids;
    }

    static std::vector<std::string> select_evenly(const std::vector<std::string>& sorted_ids, int count) {
        if (count <= 0 || sorted_ids.empty()) return {};
        if (count >= (int)sorted_ids.size()) return sorted_ids;
        std::vector<std::string> selected;
        std::set<int> used;
        if (count == 1) {
            selected.push_back(sorted_ids.front());
            return selected;
        }
        for (int i = 0; i < count; ++i) {
            double pos = (double)i * (double)(sorted_ids.size() - 1) / (double)(count - 1);
            int idx = (int)std::llround(pos);
            idx = std::max(0, std::min(idx, (int)sorted_ids.size() - 1));
            while (idx < (int)sorted_ids.size() && used.count(idx)) ++idx;
            if (idx >= (int)sorted_ids.size()) {
                idx = (int)sorted_ids.size() - 1;
                while (idx >= 0 && used.count(idx)) --idx;
            }
            if (idx >= 0 && !used.count(idx)) {
                used.insert(idx);
                selected.push_back(sorted_ids[(size_t)idx]);
            }
        }
        std::sort(selected.begin(), selected.end());
        return selected;
    }

    static std::vector<std::string> resolve_selected_ids(const std::string& selector, const std::vector<std::string>& available_ids) {
        std::string s = strip_excel_wrappers(selector);
        if (is_integer_string(s)) {
            return select_evenly(available_ids, std::atoi(s.c_str()));
        }
        std::set<std::string> available(available_ids.begin(), available_ids.end());
        std::set<std::string> seen;
        std::vector<std::string> selected;
        for (std::string id : split_ws(s)) {
            id = trim(id);
            if (id.empty()) continue;
            if (ends_with(id, "_pre.csv")) id = id.substr(0, id.size() - std::strlen("_pre.csv"));
            if (available.count(id) && seen.insert(id).second) selected.push_back(id);
        }
        return selected;
    }

    // 读取 Basin 库外对照实验配置，并解析数据选择、时间范围、采样和推断参数。
    static std::vector<ExperimentConfig> load_experiment_configs(const std::string& path) {
        if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "试验品个数或指定试验品集合,插入时间起止范围,采样步长,完整子随机过程长度T,推断时的τ,合并时的Δ,合并时的θ,MemTable容量(MB),是否使用协方差缓存驱动CDiT\n";
            out << "6112080 6114500,\"=\"\"-∞到+∞\"\"\",1,365,5,2,0.8,2,否\n";
            out << "6112080 6114500,\"=\"\"-∞到+∞\"\"\",1,365,5,2,0.8,2,是\n";
        }

        std::ifstream in(path, std::ios::binary);
        std::vector<ExperimentConfig> cfgs;
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (first) {
                first = false;
                if (line.find("试验品") != std::string::npos || line.find("MemTable") != std::string::npos) continue;
            }
            std::vector<std::string> f = parse_csv_line(line);
            if (f.size() < 8) continue;
            ExperimentConfig c;
            c.raw_sample_selector = strip_excel_wrappers(f[0]);
            c.raw_time_range = strip_excel_wrappers(f[1]);
            c.sampling_step = std::max(1, std::atoi(strip_excel_wrappers(f[2]).c_str()));
            c.window_length = std::max(1, std::atoi(strip_excel_wrappers(f[3]).c_str()));
            c.tau = std::max(1, std::atoi(strip_excel_wrappers(f[4]).c_str()));
            c.delta = std::max(1, std::atoi(strip_excel_wrappers(f[5]).c_str()));
            c.theta = std::strtod(strip_excel_wrappers(f[6]).c_str(), nullptr);
            if (!(c.theta > 0.0)) c.theta = 0.8;
            c.memtable_mb = std::max(1, std::atoi(strip_excel_wrappers(f[7]).c_str()));
            if (f.size() >= 9) c.use_covmeta_cdit = parse_yes_no_field(f[8], false);
            cfgs.push_back(c);
        }
        return cfgs;
    }

    // 加载指定 Basin 试验品，按时间范围和采样步长组织原始记录。
    static BasinDataset load_basin_dataset(const std::string& data_dir, const std::string& id, const TimeRange& tr, int sampling_step) {
        BasinDataset ds;
        ds.id = id;
        ds.path = data_dir + "/" + id + "_pre.csv";
        std::ifstream in(ds.path, std::ios::binary);
        if (!in.good()) {
            throw std::runtime_error("无法打开数据集文件: " + ds.path);
        }
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (first) {
                first = false;
                continue;
            }
            std::vector<std::string> f = parse_csv_line(line);
            if (f.size() < 4) continue;
            BasinRow row;
            row.timestamp = trim(f[0]);
            row.date_key = parse_date_key(row.timestamp);
            if (!date_in_range(row.date_key, tr)) continue;
            row.values.push_back(std::strtod(trim(f[1]).c_str(), nullptr));
            row.values.push_back(std::strtod(trim(f[2]).c_str(), nullptr));
            row.values.push_back(std::strtod(trim(f[3]).c_str(), nullptr));
            ds.rows_in_range.push_back(std::move(row));
        }
        std::sort(ds.rows_in_range.begin(), ds.rows_in_range.end(), [](const BasinRow& a, const BasinRow& b) {
            if (a.date_key != b.date_key) return a.date_key < b.date_key;
            return a.timestamp < b.timestamp;
        });
        for (size_t i = 0; i < ds.rows_in_range.size(); i += (size_t)std::max(1, sampling_step)) {
            ds.sampled_rows.push_back(&ds.rows_in_range[i]);
        }
        return ds;
    }

    // 将已准备的 Basin 数据集写入 RemixDB，供后续通过公开读取接口进行库外推断。
    static void put_dataset_to_db(struct xdb_ref* ref, const BasinDataset& ds) {
        for (const BasinRow& row : ds.rows_in_range) {
            std::string key = ds.id + "," + row.timestamp;
            std::string value = value_to_string(row.values);
            bool ok = remixdb_put(ref, key.data(), (u32)key.size(), value.data(), (u32)value.size());
            if (!ok) {
                throw std::runtime_error("remixdb_put失败: " + key);
            }
        }
    }

    static std::vector<double> read_one_row_from_db(struct xdb_ref* ref, const std::string& dataset_id, const std::string& timestamp) {
        std::string key = dataset_id + "," + timestamp;
        char value_buf[4096];
        u32 value_len = 0;
        bool ok = remixdb_get(ref, key.data(), (u32)key.size(), value_buf, &value_len);
        if (!ok) {
            throw std::runtime_error("remixdb_get未找到键: " + key);
        }
        if (value_len >= sizeof(value_buf)) {
            throw std::runtime_error("remixdb_get返回值过长: " + key);
        }
        std::string value(value_buf, value_buf + value_len);
        std::vector<double> parsed = parse_value_string(value);
        if (parsed.size() != 3) {
            throw std::runtime_error("数据库值无法解析为3列: " + key + " => " + value);
        }
        return parsed;
    }

    static std::string format_graph_for_basin(const CDiTResult& graph) {
        static const std::vector<std::string> names = {"T", "P", "Q"};
        return format_inferred_named(graph, names);
    }

    // 根据当前配置选择原始 CDiT 或统计量缓存驱动的 CDiT 计算路径。
    static CDiTResult run_configured_cdit(const std::vector<std::vector<double>>& window_data, const ExperimentConfig& cfg) {
        if (cfg.use_covmeta_cdit) {
            return run_cdit_covmeta(window_data, cfg.tau, 0.01, false, nullptr, nullptr);
        }
        return run_cdit_original(window_data, cfg.tau, 0.01, false, nullptr, nullptr);
    }

    static MergeCGResult merge_adjacent_causal_graphs_timed(const CDiTResult& left,
                                                            const CDiTResult& right,
                                                            int tau,
                                                            int delta,
                                                            double theta,
                                                            bool ignore_self,
                                                            double* merge_seconds) {
        const auto merge_t0 = Clock::now();
        MergeCGResult mr = merge_adjacent_causal_graphs(left, right, tau, delta, theta, ignore_self);
        if (merge_seconds) {
            *merge_seconds += seconds_since(merge_t0);
        }
        return mr;
    }

    // 将新窗口结果按时间顺序插入分段集合，并按阈值反复尝试与相邻段合并。
    static void insert_segment_with_repeated_merge(std::vector<GraphSegment>& entries,
                                                   GraphSegment seg,
                                                   int tau,
                                                   int delta,
                                                   double theta,
                                                   double* merge_seconds) {
        entries.push_back(std::move(seg));
        size_t idx = entries.size() - 1;
        bool changed = true;
        while (changed && idx < entries.size()) {
            changed = false;
            if (idx > 0) {
                MergeCGResult mr = merge_adjacent_causal_graphs_timed(entries[idx - 1].graph, entries[idx].graph,
                                                                      tau, delta, theta, true, merge_seconds);
                if (mr.merge) {
                    GraphSegment merged;
                    merged.start_label = entries[idx - 1].start_label;
                    merged.end_label = entries[idx].end_label;
                    merged.graph = std::move(mr.merged);
                    entries.erase(entries.begin() + (long)idx - 1, entries.begin() + (long)idx + 1);
                    entries.insert(entries.begin() + (long)idx - 1, std::move(merged));
                    --idx;
                    changed = true;
                    continue;
                }
            }
            if (idx + 1 < entries.size()) {
                MergeCGResult mr = merge_adjacent_causal_graphs_timed(entries[idx].graph, entries[idx + 1].graph,
                                                                      tau, delta, theta, true, merge_seconds);
                if (mr.merge) {
                    GraphSegment merged;
                    merged.start_label = entries[idx].start_label;
                    merged.end_label = entries[idx + 1].end_label;
                    merged.graph = std::move(mr.merged);
                    entries.erase(entries.begin() + (long)idx, entries.begin() + (long)idx + 2);
                    entries.insert(entries.begin() + (long)idx, std::move(merged));
                    changed = true;
                    continue;
                }
            }
        }
    }

    static void write_stats_header(std::ofstream& out) {
        // 写入 UTF-8 BOM，便于 Excel 正确识别中文表头和无穷大符号。
        out << "\xEF\xBB\xBF";
        out << "试验品个数或指定试验品集合,插入时间起止范围,采样步长,完整子随机过程长度T,推断时的τ,合并时的Δ,合并时的θ,MemTable容量(MB),是否使用协方差缓存驱动CDiT,总耗时(s),插入总耗时(s),合并压缩率,库外推断中仅读取部分耗时(s),库外推断中仅推断部分耗时(s),仅合并部分耗时(s)\n";
    }

    static void append_stats_row(std::ofstream& out,
                                 const ExperimentConfig& cfg,
                                 const TimeRange& tr,
                                 double total_seconds,
                                 double insert_seconds,
                                 double compression_percent,
                                 double read_seconds,
                                 double infer_seconds,
                                 double merge_seconds) {
        out << csv_escape(strip_excel_wrappers(cfg.raw_sample_selector)) << ','
            << excel_formula_text_field(tr.display) << ','
            << cfg.sampling_step << ','
            << cfg.window_length << ','
            << cfg.tau << ','
            << cfg.delta << ','
            << format_compact_double(cfg.theta) << ','
            << cfg.memtable_mb << ','
            << yes_no_text(cfg.use_covmeta_cdit) << ','
            << format_double(total_seconds) << ','
            << format_double(insert_seconds) << ','
            << format_percent(compression_percent) << ','
            << format_double(read_seconds) << ','
            << format_double(infer_seconds) << ','
            << format_double(merge_seconds) << "\n";
        out.flush();
    }

} // 匿名命名空间结束

// Basin 入库与库外推断对照实验入口，分别统计写入、读取、推断和图合并耗时。
extern "C" int run_outdb_basin_experiments(void) {
    const std::string data_dir = "data/basin_data";
    const std::string input_path = "experiment/outdb_basin_experiment_input.csv";
    const std::string log_dir = "experiment_log";
    const std::string print_log_path = log_dir + "/河川径流入库+库外推断实验全部打印日志.txt";
    const std::string stats_path = log_dir + "/对河川径流的入库+库外推断实验统计.csv";
    const std::string db_dir = "xdb_basin_outdb_experiment";

    std::filesystem::create_directories(log_dir);
    std::filesystem::create_directories("file/basin_experiment");
    TeeLogger logger(print_log_path);

    std::ofstream stats(stats_path, std::ios::binary | std::ios::trunc);
    write_stats_header(stats);

    std::vector<ExperimentConfig> configs = load_experiment_configs(input_path);
    std::vector<std::string> available_ids = list_basin_ids(data_dir);
    if (configs.empty()) {
        logger.line("Basin 入库+库外推断实验输入配置为空，跳过该实验。");
        return 0;
    }
    if (available_ids.empty()) {
        logger.line("未在 data/basin_data 中找到 *_pre.csv 文件，跳过 Basin 入库+库外推断实验。");
        return 0;
    }

    for (size_t exp_idx = 0; exp_idx < configs.size(); ++exp_idx) {
        const ExperimentConfig& cfg = configs[exp_idx];
        TimeRange tr = parse_time_range(cfg.raw_time_range);
        std::vector<std::string> selected_ids = resolve_selected_ids(cfg.raw_sample_selector, available_ids);

        // 清理操作与元数据开关重置均放在计时区间之外，避免计入实验耗时。
        std::filesystem::remove_all(db_dir);
        std::filesystem::create_directories(db_dir);
        causal_meta_reset_all();
        causal_meta_set_enabled(0);

        logger.line("河川径流第" + std::to_string(exp_idx + 1) + "次实验");
        logger.line("数据库参数设置：MemTable=" + std::to_string(cfg.memtable_mb) + "MB");
        logger.line("本此插入实验参数设置：插入时间范围=" + tr.display +
                    "；要插入的试验品个数=" + std::to_string(selected_ids.size()) +
                    "；是否使用协方差缓存驱动CDiT：" + yes_no_text(cfg.use_covmeta_cdit) + "；");
        logger.line();

        double insert_seconds = 0.0;
        double read_seconds = 0.0;
        double infer_seconds = 0.0;
        double merge_seconds = 0.0;
        long long original_window_count = 0;
        long long final_segment_count = 0;
        double total_seconds = 0.0;

        try {
            std::vector<BasinDataset> datasets;
            datasets.reserve(selected_ids.size());
            for (const std::string& id : selected_ids) {
                BasinDataset ds = load_basin_dataset(data_dir, id, tr, cfg.sampling_step);
                if (!ds.rows_in_range.empty()) datasets.push_back(std::move(ds));
            }

            auto total_t0 = Clock::now();
            auto insert_t0 = Clock::now();
            {
                RemixSession insert_db(db_dir, cfg.memtable_mb);
                for (const BasinDataset& ds : datasets) {
                    put_dataset_to_db(insert_db.ref, ds);
                }
                insert_db.sync();
            }
            insert_seconds = seconds_since(insert_t0);

            // 关闭后重新打开数据库，使所有库外读取均通过 RemixDB 的公开读取接口完成。
            RemixSession read_db(db_dir, cfg.memtable_mb);

            logger.line("本次范围推断输入：实验品个数=" + std::to_string(datasets.size()) +
                        "；推断时间区间=-∞到+∞；参与列=T,P,Q；采样步长=" + std::to_string(cfg.sampling_step) +
                        "；T=" + std::to_string(cfg.window_length) +
                        "；τ=" + std::to_string(cfg.tau) +
                        "；Δ=" + std::to_string(cfg.delta) +
                        "；θ=" + format_compact_double(cfg.theta));

            for (const BasinDataset& ds : datasets) {
                logger.line("试验品：" + ds.id);
                std::vector<GraphSegment> segments;
                const size_t n_complete_windows = ds.sampled_rows.size() / (size_t)cfg.window_length;
                original_window_count += (long long)n_complete_windows;

                for (size_t w = 0; w < n_complete_windows; ++w) {
                    std::vector<std::vector<double>> window_data;
                    window_data.reserve((size_t)cfg.window_length);
                    std::string start_label;
                    std::string end_label;

                    auto read_t0 = Clock::now();
                    for (int j = 0; j < cfg.window_length; ++j) {
                        const BasinRow* row = ds.sampled_rows[w * (size_t)cfg.window_length + (size_t)j];
                        if (j == 0) start_label = row->timestamp;
                        if (j == cfg.window_length - 1) end_label = row->timestamp;
                        window_data.push_back(read_one_row_from_db(read_db.ref, ds.id, row->timestamp));
                    }
                    read_seconds += seconds_since(read_t0);

                    auto infer_t0 = Clock::now();
                    CDiTResult graph = run_configured_cdit(window_data, cfg);
                    infer_seconds += seconds_since(infer_t0);

                    GraphSegment seg;
                    seg.start_label = start_label;
                    seg.end_label = end_label;
                    seg.graph = std::move(graph);
                    insert_segment_with_repeated_merge(segments, std::move(seg), cfg.tau, cfg.delta, cfg.theta, &merge_seconds);
                }

                final_segment_count += (long long)segments.size();
                for (const GraphSegment& seg : segments) {
                    logger.line(seg.start_label + "至" + seg.end_label + " | 推断：" + format_graph_for_basin(seg.graph));
                }
            }

            read_db.close();
            total_seconds = seconds_since(total_t0);

            double compression = 0.0;
            if (original_window_count > 0) {
                compression = 100.0 * (double)(original_window_count - final_segment_count) / (double)original_window_count;
            }
            logger.line("本次范围查询小节：耗时:" + format_double(read_seconds + infer_seconds + merge_seconds) +
                        "s，合并压缩率:" + format_percent(compression));
            logger.line();

            append_stats_row(stats, cfg, tr, total_seconds, insert_seconds, compression, read_seconds, infer_seconds, merge_seconds);
        } catch (const std::exception& ex) {
            total_seconds = 0.0;
            logger.line(std::string("实验失败：") + ex.what());
            logger.line();
            append_stats_row(stats, cfg, tr, total_seconds, insert_seconds, 0.0, read_seconds, infer_seconds, merge_seconds);
        }

        // 每组实验结束后的清理操作不计入测量时间。
        std::filesystem::remove_all(db_dir);
        causal_meta_reset_all();
        causal_meta_set_enabled(0);
    }

    return 0;
}
