/*
 * 本文件实现 WindTunnel 数据集的库外处理对照实验，负责数据写入、重新读取、窗口分析和分项计时。
 */

#include "experiment/outdb_windtunnel_experiment.h"

#include "causal_meta.h"
#include "cdit_module.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct ColumnSpec {
    std::string alias;
    std::string csv_name;
    bool is_cause = false;
};

struct DatasetSpec {
    std::string alias;
    std::string dataset_id;
};

struct ExperimentCase {
    std::vector<std::string> requested_dataset_tokens;

    std::string time_range_text;
    std::optional<double> start_time;
    std::optional<double> end_time;

    std::vector<ColumnSpec> cause_columns;
    std::vector<ColumnSpec> result_columns;
    int sampling_step = 1;
    int window_length = 365;
    int tau = 5;
    int memtable_mb = 2;
    bool use_covmeta = false;
};

struct TimestampInfo {
    std::string key;
    std::string label;
};

struct DatasetInfo {
    DatasetSpec spec;
    fs::path path;
    std::vector<TimestampInfo> timestamps;
    std::unordered_set<std::string> allowed_timestamp_keys;
};

struct ExperimentPaths {
    fs::path root;
    fs::path data_dir;
    fs::path input_csv;
    fs::path db_dir;
    fs::path log_dir;
    fs::path summary_csv;
    fs::path print_log;
};

struct InferenceSegment {
    std::string start_label;
    std::string end_label;
    CDiTResult graph;
};

struct QueryTiming {
    double total_seconds = 0.0;
    double read_seconds = 0.0;
    double inference_seconds = 0.0;
};

class ExperimentLogger {
public:
    explicit ExperimentLogger(fs::path print_log) : print_log_(std::move(print_log)) {}

    void initialize() {
        fs::create_directories(print_log_.parent_path());
        stream_.close();
        stream_.clear();
        stream_.open(print_log_, std::ios::binary | std::ios::trunc);
        if (!stream_) throw std::runtime_error("无法创建打印日志: " + print_log_.string());
        // UTF-8 BOM，便于 Windows Excel/记事本直接识别中文。
        stream_.write("\xEF\xBB\xBF", 3);
        stream_.flush();
    }

    void line(const std::string& text = std::string()) {
        std::cout << text << '\n';
        if (stream_) {
            stream_ << text << '\n';
            // 每打印一行就落盘，实验尚未全部结束时也可直接查看已有日志。
            stream_.flush();
        }
    }

private:
    fs::path print_log_;
    std::ofstream stream_;
};

static const std::vector<ColumnSpec>& all_columns() {
    static const std::vector<ColumnSpec> kColumns = {
        {"H", "hatch", true},
        {"A1", "pot_1", true},
        {"A2", "pot_2", true},
        {"Lin", "load_in", true},
        {"Lout", "load_out", true},
        {"R1", "v_1", true},
        {"R2", "v_2", true},
        {"RM", "v_mic", true},
        {"Rin", "v_in", true},
        {"Rout", "v_out", true},
        {"Cin", "current_in", false},
        {"Cout", "current_out", false},
        {"ωin", "rpm_in", false},
        {"ωout", "rpm_out", false},
        {"Pup", "pressure_upwind", false},
        {"Pdw", "pressure_downwind", false},
        {"Pamb", "pressure_ambient", false},
        {"Pint", "pressure_intake", false},
        {"M", "mic", false},
        {"S1", "signal_1", false},
        {"S2", "signal_2", false},
    };
    return kColumns;
}

static const std::vector<DatasetSpec>& dataset_registry() {
    static const std::vector<DatasetSpec> kDatasets = {
        {"w1", "actuators_random_walk_1"},
        {"w2", "actuators_random_walk_2"},
        {"w3", "actuators_random_walk_3"},
        {"w4", "actuators_random_walk_4"},
        {"w5", "actuators_random_walk_5"},
        {"w6", "actuators_random_walk_6"},
        {"w7", "actuators_random_walk_7"},
        {"w8", "actuators_random_walk_8"},
        {"w9", "actuators_random_walk_9"},
        {"w10", "actuators_random_walk_10"},
        {"w11", "actuators_random_walk_11"},
        {"w12", "actuators_random_walk_12"},
        {"w13", "actuators_random_walk_13"},
        {"w14", "actuators_random_walk_14"},
        {"w15", "actuators_random_walk_15"},
        {"w16", "actuators_random_walk_16"},
        {"w", "actuators_random_walk"},
        {"f1", "loads_hatch_mix_fast_run_1"},
        {"f2", "loads_hatch_mix_fast_run_2"},
        {"f3", "loads_hatch_mix_fast_run_3"},
        {"f4", "loads_hatch_mix_fast_run_4"},
        {"f5", "loads_hatch_mix_fast_run_5"},
        {"f", "loads_hatch_mix_fast_run"},
        {"s1", "loads_hatch_mix_slow_run_1"},
        {"s2", "loads_hatch_mix_slow_run_2"},
        {"s3", "loads_hatch_mix_slow_run_3"},
        {"s4", "loads_hatch_mix_slow_run_4"},
        {"s5", "loads_hatch_mix_slow_run_5"},
        {"s", "loads_hatch_mix_slow_run"},
        {"mul", "regime_jumps_multi"},
        {"sin", "regime_jumps_single"},
    };
    return kDatasets;
}

static double seconds_between(const Clock::time_point& a, const Clock::time_point& b) {
    return std::chrono::duration<double>(b - a).count();
}

static std::string trim(const std::string& s) {
    std::size_t l = 0;
    std::size_t r = s.size();
    while (l < r && std::isspace(static_cast<unsigned char>(s[l]))) ++l;
    while (r > l && std::isspace(static_cast<unsigned char>(s[r - 1]))) --r;
    return s.substr(l, r - l);
}

static std::string remove_utf8_bom(std::string s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

static void replace_all(std::string* s, const std::string& from, const std::string& to) {
    if (!s || from.empty()) return;
    std::size_t p = 0;
    while ((p = s->find(from, p)) != std::string::npos) {
        s->replace(p, from.size(), to);
        p += to.size();
    }
}

// RFC4180 风格单行 CSV 解析；支持 Excel 双引号转义。
static std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cur.push_back(c);
            }
        } else {
            if (c == '"') {
                quoted = true;
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
    bool need_quotes = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            need_quotes = true;
            break;
        }
    }
    if (!need_quotes) return s;

    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

static std::string normalize_excel_cell(std::string s) {
    s = trim(s);
    // parse_csv_line 已经去掉 CSV 字段最外层引号；这里继续处理 Excel 的 ="..."。
    if (s.size() >= 3 && s[0] == '=' && s[1] == '"' && s.back() == '"') {
        s = s.substr(2, s.size() - 3);
    }
    // 同时容忍 Excel/人工编辑后只剩一层普通引号的情况。
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return trim(s);
}

static std::string excel_safe_time_range(const std::string& s) {
    // -∞到+∞ 直接写 CSV 时，Excel 可能当公式；输出 ="..." 后再做 RFC4180 转义。
    if (!s.empty() && s.front() == '-') return "=\"" + s + "\"";
    return s;
}

static std::string ascii_lower(std::string s) {
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 128) c = static_cast<char>(std::tolower(u));
    }
    return s;
}

static bool parse_yes_no(const std::string& raw, bool* out) {
    const std::string s = normalize_excel_cell(raw);
    const std::string low = ascii_lower(s);
    if (s == "是" || low == "yes" || low == "true" || s == "1") {
        *out = true;
        return true;
    }
    if (s == "否" || low == "no" || low == "false" || s == "0") {
        *out = false;
        return true;
    }
    return false;
}

static std::string yesno(bool v) { return v ? "是" : "否"; }

static bool parse_positive_int(const std::string& raw, int* out) {
    const std::string s = normalize_excel_cell(raw);
    if (s.empty()) return false;
    try {
        std::size_t used = 0;
        const int v = std::stoi(s, &used);
        if (used != s.size() || v <= 0) return false;
        *out = v;
        return true;
    } catch (...) {
        return false;
    }
}

static bool parse_finite_double(const std::string& raw, double* out) {
    const std::string s = trim(raw);
    if (s.empty()) return false;
    try {
        std::size_t used = 0;
        const double v = std::stod(s, &used);
        while (used < s.size() && std::isspace(static_cast<unsigned char>(s[used]))) ++used;
        if (used != s.size() || !std::isfinite(v)) return false;
        *out = v;
        return true;
    } catch (...) {
        return false;
    }
}

static std::string format_time_2(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << v;
    return oss.str();
}

static std::string canonical_timestamp_key(double v) {
    // 使用数据集自己的 timestamp 数值生成稳定 key；不是另造递增时间戳。
    std::ostringstream oss;
    oss << std::setprecision(17) << v;
    return oss.str();
}

static bool parse_time_boundary(const std::string& raw,
                                bool is_start,
                                std::optional<double>* value) {
    const std::string s = ascii_lower(trim(raw));
    if ((is_start && (s == "-∞" || s == "-inf" || s == "-infinity")) ||
        (!is_start && (s == "+∞" || s == "∞" || s == "+inf" || s == "inf" ||
                       s == "+infinity" || s == "infinity"))) {
        value->reset();
        return true;
    }
    double v = 0.0;
    if (!parse_finite_double(s, &v)) return false;
    *value = v;
    return true;
}

static bool parse_time_range(const std::string& raw,
                             std::string* normalized,
                             std::optional<double>* start,
                             std::optional<double>* end,
                             std::string* error) {
    const std::string s = normalize_excel_cell(raw);
    const std::string marker = "到";
    const std::size_t p = s.find(marker);
    if (p == std::string::npos) {
        if (error) *error = "无法解析时间范围: " + raw;
        return false;
    }

    std::optional<double> a;
    std::optional<double> b;
    if (!parse_time_boundary(s.substr(0, p), true, &a) ||
        !parse_time_boundary(s.substr(p + marker.size()), false, &b)) {
        if (error) *error = "时间范围非法: " + raw;
        return false;
    }
    if (a && b && *a > *b) {
        if (error) *error = "时间范围起点晚于终点: " + raw;
        return false;
    }

    *start = a;
    *end = b;
    *normalized = (a ? format_time_2(*a) : "-∞") + marker +
                  (b ? format_time_2(*b) : "+∞");
    return true;
}

static bool time_in_range(double ts,
                          const std::optional<double>& start,
                          const std::optional<double>& end) {
    if (start && ts < *start) return false;
    if (end && ts > *end) return false;
    return true;
}

static std::vector<std::string> split_list_field(std::string s) {
    s = normalize_excel_cell(std::move(s));
    replace_all(&s, "，", " ");
    replace_all(&s, "；", " ");
    replace_all(&s, "、", " ");
    for (char& c : s) {
        if (c == ',' || c == ';' || c == '\t' || c == '\r' || c == '\n') c = ' ';
    }
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string x;
    while (iss >> x) out.push_back(x);
    return out;
}

static const ColumnSpec* find_column_spec(const std::string& token) {
    const std::string t = trim(token);
    const std::string low = ascii_lower(t);
    for (const ColumnSpec& c : all_columns()) {
        if (t == c.alias || t == c.csv_name || low == ascii_lower(c.alias) ||
            low == ascii_lower(c.csv_name)) {
            return &c;
        }
    }
    return nullptr;
}

static std::vector<ColumnSpec> parse_column_list(const std::string& raw,
                                                 bool want_cause,
                                                 int line_no) {
    const std::vector<std::string> tokens = split_list_field(raw);
    if (tokens.empty()) {
        throw std::runtime_error("第 " + std::to_string(line_no) + " 行的" +
                                 (want_cause ? "原因列" : "结果列") + "为空");
    }

    std::vector<ColumnSpec> out;
    std::unordered_set<std::string> seen;
    for (const std::string& token : tokens) {
        const ColumnSpec* spec = find_column_spec(token);
        if (!spec) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行存在未知列: " + token);
        }
        if (spec->is_cause != want_cause) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行的列 " + spec->alias +
                                     (want_cause ? " 不是可操纵原因列" : " 不是传感器结果列"));
        }
        if (seen.insert(spec->alias).second) out.push_back(*spec);
    }
    return out;
}

static std::vector<ColumnSpec> storage_columns(const ExperimentCase& c) {
    // 库外实验只把本次指定的原因列和结果列写入 value；顺序固定为“原因列在前、结果列在后”。
    std::vector<ColumnSpec> out;
    out.reserve(c.cause_columns.size() + c.result_columns.size());
    out.insert(out.end(), c.cause_columns.begin(), c.cause_columns.end());
    out.insert(out.end(), c.result_columns.begin(), c.result_columns.end());
    return out;
}

static std::string join_strings(const std::vector<std::string>& xs, const std::string& delim) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (i) oss << delim;
        oss << xs[i];
    }
    return oss.str();
}

static std::string join_column_aliases(const std::vector<ColumnSpec>& xs) {
    std::vector<std::string> names;
    names.reserve(xs.size());
    for (const ColumnSpec& x : xs) names.push_back(x.alias);
    return join_strings(names, ",");
}

static ExperimentPaths resolve_paths() {
    ExperimentPaths p;
    p.root = fs::current_path();
    p.input_csv = p.root / "experiment" / "outdb_windtunnel_experiment_input.csv";
    p.db_dir = p.root / "rdb_windtunnel_outdb_experiment";
    p.log_dir = p.root / "experiment_log";
    p.summary_csv = p.log_dir / "对风洞实验的入库+库外推断实验统计.csv";
    p.print_log = p.log_dir / "风洞实验入库+库外推断实验全部打印日志.txt";

    if (const char* env = std::getenv("WINDTUNNEL_DATA_DIR")) {
        if (*env) p.data_dir = fs::path(env);
    }
    if (const char* env = std::getenv("OUTDB_WINDTUNNEL_EXPERIMENT_INPUT")) {
        if (*env) p.input_csv = fs::path(env);
    }
    if (p.data_dir.empty()) {
        const std::vector<fs::path> candidates = {
            p.root / ".." / "remixdb" / "data" / "windtunnel_data",
            p.root / "data" / "windtunnel_data",
            p.root / ".." / "remixdb" / "data" / "windtunnel_data"
        };
        for (const fs::path& c : candidates) {
            std::error_code ec;
            if (fs::is_directory(c, ec)) {
                p.data_dir = c;
                break;
            }
        }
    }
    return p;
}

static std::map<std::string, fs::path> discover_clean_data(const fs::path& root) {
    if (root.empty() || !fs::is_directory(root)) {
        throw std::runtime_error("找不到 windtunnel_data 目录；可设置 WINDTUNNEL_DATA_DIR。当前目录: " +
                                 root.string());
    }

    std::map<std::string, fs::path> out;
    for (const auto& ent : fs::recursive_directory_iterator(root)) {
        if (!ent.is_regular_file()) continue;
        const std::string name = ent.path().filename().string();
        static const std::string suffix = "_pre.csv";
        if (name.size() <= suffix.size() ||
            name.substr(name.size() - suffix.size()) != suffix) {
            continue;
        }
        const std::string dataset_id = name.substr(0, name.size() - suffix.size());
        if (!dataset_id.empty()) out[dataset_id] = ent.path();
    }
    if (out.empty()) throw std::runtime_error("windtunnel_data 中没有发现 *_pre.csv");
    return out;
}

static std::vector<ExperimentCase> load_cases(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开实验输入文件: " + path.string());

    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("实验输入文件为空: " + path.string());
    line = remove_utf8_bom(line);

    std::vector<ExperimentCase> out;
    int line_no = 1;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        const std::vector<std::string> f = parse_csv_line(line);
        if (f.size() < 9) {
            throw std::runtime_error("outdb_windtunnel_experiment_input.csv 第 " +
                                     std::to_string(line_no) + " 行少于 9 列");
        }

        ExperimentCase c;
        c.requested_dataset_tokens = split_list_field(f[0]);
        if (c.requested_dataset_tokens.empty()) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行指定试验品集合为空");
        }

        std::string error;
        if (!parse_time_range(f[1], &c.time_range_text, &c.start_time, &c.end_time, &error)) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行: " + error);
        }
        c.cause_columns = parse_column_list(f[2], true, line_no);
        c.result_columns = parse_column_list(f[3], false, line_no);

        if (!parse_positive_int(f[4], &c.sampling_step) ||
            !parse_positive_int(f[5], &c.window_length) ||
            !parse_positive_int(f[6], &c.tau) ||
            !parse_positive_int(f[7], &c.memtable_mb)) {
            throw std::runtime_error("第 " + std::to_string(line_no) +
                                     " 行采样步长、T、τ、MemTable(MB) 必须为正整数");
        }
        if (c.window_length < 2 * c.tau) {
            throw std::runtime_error("第 " + std::to_string(line_no) +
                                     " 行 T < 2*τ，CDiT 没有有效样本");
        }
        if (!parse_yes_no(f[8], &c.use_covmeta)) {
            throw std::runtime_error("第 " + std::to_string(line_no) +
                                     " 行‘是否使用协方差缓存驱动CDiT’无法识别");
        }
        out.push_back(std::move(c));
    }

    if (out.empty()) throw std::runtime_error("实验输入文件没有有效实验行");
    return out;
}

static std::vector<DatasetSpec> resolve_datasets(const std::map<std::string, fs::path>& discovered,
                                                 const ExperimentCase& c) {
    std::map<std::string, DatasetSpec> by_alias;
    std::map<std::string, DatasetSpec> by_id;
    for (const DatasetSpec& s : dataset_registry()) {
        by_alias[ascii_lower(s.alias)] = s;
        by_id[ascii_lower(s.dataset_id)] = s;
    }

    bool all = false;
    for (const std::string& token0 : c.requested_dataset_tokens) {
        const std::string token = ascii_lower(trim(token0));
        if (token == "all" || token == "*" || token0 == "全部") all = true;
    }

    std::vector<DatasetSpec> out;
    std::unordered_set<std::string> seen;
    if (all) {
        for (const DatasetSpec& s : dataset_registry()) {
            if (discovered.count(s.dataset_id)) {
                out.push_back(s);
                seen.insert(s.dataset_id);
            }
        }
    } else {
        for (const std::string& token0 : c.requested_dataset_tokens) {
            const std::string token = ascii_lower(trim(token0));
            DatasetSpec spec;
            bool found = false;
            const auto ait = by_alias.find(token);
            if (ait != by_alias.end()) {
                spec = ait->second;
                found = true;
            } else {
                const auto dit = by_id.find(token);
                if (dit != by_id.end()) {
                    spec = dit->second;
                    found = true;
                }
            }
            if (!found) throw std::runtime_error("未知风洞试验品简写/数据集名: " + token0);
            if (!discovered.count(spec.dataset_id)) {
                throw std::runtime_error("指定试验品缺少文件: " + spec.alias + " (" +
                                         spec.dataset_id + "_pre.csv)");
            }
            if (seen.insert(spec.dataset_id).second) out.push_back(spec);
        }
    }

    if (out.empty()) throw std::runtime_error("本次实验没有可用风洞试验品");
    return out;
}

static std::vector<std::string> dataset_aliases(const std::vector<DatasetSpec>& datasets) {
    std::vector<std::string> out;
    out.reserve(datasets.size());
    for (const DatasetSpec& x : datasets) out.push_back(x.alias);
    return out;
}

static std::map<std::string, int> header_index(const std::vector<std::string>& header) {
    std::map<std::string, int> out;
    for (std::size_t i = 0; i < header.size(); ++i) {
        out[trim(header[i])] = static_cast<int>(i);
    }
    return out;
}

static DatasetInfo load_dataset_info(const DatasetSpec& spec,
                                     const fs::path& path,
                                     const ExperimentCase& c) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开数据集: " + path.string());

    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("空数据集: " + path.string());
    const std::map<std::string, int> idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    const auto tit = idx.find("timestamp");
    if (tit == idx.end()) throw std::runtime_error("数据集缺少 timestamp 列: " + path.string());
    for (const ColumnSpec& col : storage_columns(c)) {
        if (!idx.count(col.csv_name)) {
            throw std::runtime_error("数据集缺少列 " + col.csv_name + ": " + path.string());
        }
    }

    DatasetInfo info;
    info.spec = spec;
    info.path = path;
    const int time_col = tit->second;
    bool have_last = false;
    double last_ts = 0.0;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::vector<std::string> f = parse_csv_line(line);
        if (time_col >= static_cast<int>(f.size())) {
            throw std::runtime_error("数据集存在列数不足的数据行: " + path.string());
        }
        double ts = 0.0;
        if (!parse_finite_double(f[static_cast<std::size_t>(time_col)], &ts)) {
            throw std::runtime_error("无法解析 timestamp: " +
                                     f[static_cast<std::size_t>(time_col)] + " / " + path.string());
        }
        if (!time_in_range(ts, c.start_time, c.end_time)) continue;
        if (have_last && !(ts > last_ts)) {
            throw std::runtime_error("数据集时间戳在所选范围内不是严格递增: " + spec.dataset_id +
                                     " / " + format_time_2(ts));
        }
        have_last = true;
        last_ts = ts;

        const std::string key = canonical_timestamp_key(ts);
        if (!info.allowed_timestamp_keys.insert(key).second) {
            throw std::runtime_error("数据集出现重复 timestamp: " + spec.dataset_id + " / " + key);
        }
        info.timestamps.push_back({key, format_time_2(ts)});
    }

    // 时间范围与该试验品无交集时，按“本试验品本轮零插入、零完整窗口”处理。
    // 不能在这里抛异常：insert_dataset() 已支持 0 行插入，库外推断也会自然得到 0 个窗口，
    // 从而保证其它试验品仍可继续，并且本次实验最终仍能正常写入统计日志。
    return info;
}

static void insert_dataset(rocksdb::DB* db,
                           const DatasetInfo& info,
                           const ExperimentCase& c) {
    std::ifstream in(info.path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开插入文件: " + info.path.string());

    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("插入文件为空: " + info.path.string());
    const std::map<std::string, int> idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    if (!idx.count("timestamp")) {
        throw std::runtime_error("插入文件缺少 timestamp 列: " + info.path.string());
    }

    const std::vector<ColumnSpec> cols = storage_columns(c);
    std::vector<int> value_cols;
    value_cols.reserve(cols.size());
    int max_col = idx.at("timestamp");
    for (const ColumnSpec& col : cols) {
        const auto it = idx.find(col.csv_name);
        if (it == idx.end()) {
            throw std::runtime_error("插入文件缺少列 " + col.csv_name + ": " + info.path.string());
        }
        value_cols.push_back(it->second);
        max_col = std::max(max_col, it->second);
    }

    rocksdb::WriteOptions wopt;
    std::size_t inserted = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::vector<std::string> f = parse_csv_line(line);
        if (max_col >= static_cast<int>(f.size())) {
            throw std::runtime_error("插入文件存在列数不足的数据行: " + info.path.string());
        }

        double ts = 0.0;
        if (!parse_finite_double(f[static_cast<std::size_t>(idx.at("timestamp"))], &ts)) {
            throw std::runtime_error("插入文件存在非法 timestamp: " + info.path.string());
        }
        const std::string ts_key = canonical_timestamp_key(ts);
        if (!info.allowed_timestamp_keys.count(ts_key)) continue;

        std::ostringstream value;
        for (std::size_t i = 0; i < value_cols.size(); ++i) {
            if (i) value << ',';
            const std::string cell = trim(f[static_cast<std::size_t>(value_cols[i])]);
            value << (cell.empty() ? "0" : cell);
        }

        const std::string key = info.spec.dataset_id + "," + ts_key;
        const rocksdb::Status s = db->Put(wopt, key, value.str());
        if (!s.ok()) {
            throw std::runtime_error("RocksDB DB::Put 写入失败: key=" + key + " / " + s.ToString());
        }
        ++inserted;
    }

    if (inserted != info.timestamps.size()) {
        throw std::runtime_error("插入行数与预扫描行数不一致: " + info.spec.dataset_id +
                                 "，预期=" + std::to_string(info.timestamps.size()) +
                                 "，实际=" + std::to_string(inserted));
    }
}

static std::vector<double> parse_db_value(const std::string& value,
                                          std::size_t expected_columns,
                                          const std::string& dataset_id,
                                          const std::string& ts_key) {
    std::vector<double> out;
    out.reserve(expected_columns);
    std::size_t begin = 0;
    while (true) {
        const std::size_t p = value.find(',', begin);
        const std::string cell = trim(value.substr(begin, p == std::string::npos ?
                                                           std::string::npos : p - begin));
        double v = 0.0;
        if (!parse_finite_double(cell, &v)) {
            throw std::runtime_error("RocksDB value 数值解析失败: " + dataset_id + " / " +
                                     ts_key + " / value=" + value);
        }
        out.push_back(v);
        if (p == std::string::npos) break;
        begin = p + 1;
    }

    if (out.size() != expected_columns) {
        throw std::runtime_error("RocksDB value 列数不正确: " + dataset_id + " / " + ts_key +
                                 "，预期=" + std::to_string(expected_columns) +
                                 "，实际=" + std::to_string(out.size()));
    }
    return out;
}

static void destroy_db_state(const fs::path& db_dir, const rocksdb::Options& options) {
    // RocksDB 官方公开接口 DestroyDB 必须在 DB 已关闭时调用；它负责清理该 DB 的标准持久化状态。
    std::error_code exists_ec;
    const bool exists = fs::exists(db_dir, exists_ec);
    if (exists_ec) {
        throw std::runtime_error("检查数据库目录失败: " + db_dir.string() + " / " +
                                 exists_ec.message());
    }
    if (!exists) return;

    const rocksdb::Status s = rocksdb::DestroyDB(db_dir.string(), options);
    if (!s.ok() && !s.IsNotFound()) {
        throw std::runtime_error("RocksDB DestroyDB 清库失败: " + s.ToString());
    }

    // 额外移除异常强退遗留、但不一定属于 RocksDB 标准文件集合的内容。
    std::error_code ec;
    fs::remove_all(db_dir, ec);
    if (ec) {
        throw std::runtime_error("清理数据库残留目录失败: " + db_dir.string() + " / " + ec.message());
    }
}

static void destroy_db_state_noexcept(const fs::path& db_dir, const rocksdb::Options& options) {
    try {
        destroy_db_state(db_dir, options);
    } catch (...) {
        // 异常传播路径只做 best-effort 清理，不覆盖最先出现的实验异常。
    }
}

static std::vector<TimestampInfo> sampled_timestamps(const DatasetInfo& info, int sampling_step) {
    std::vector<TimestampInfo> out;
    out.reserve((info.timestamps.size() + static_cast<std::size_t>(sampling_step) - 1) /
                static_cast<std::size_t>(sampling_step));
    for (std::size_t raw_pos = 0; raw_pos < info.timestamps.size(); ++raw_pos) {
        if (raw_pos % static_cast<std::size_t>(sampling_step) == 0) {
            out.push_back(info.timestamps[raw_pos]);
        }
    }
    return out;
}

static CDiTResult canonical_graph(CDiTResult g) {
    for (auto& kv : g.lags_by_edge) {
        std::vector<int>& lags = kv.second;
        std::sort(lags.begin(), lags.end());
        lags.erase(std::unique(lags.begin(), lags.end()), lags.end());
    }
    for (auto it = g.lags_by_edge.begin(); it != g.lags_by_edge.end();) {
        if (it->second.empty()) it = g.lags_by_edge.erase(it);
        else ++it;
    }
    return g;
}

static bool same_graph(const CDiTResult& a, const CDiTResult& b) {
    return canonical_graph(a).lags_by_edge == canonical_graph(b).lags_by_edge;
}

static void append_or_merge_segment(std::vector<InferenceSegment>* segments,
                                    const std::string& start_label,
                                    const std::string& end_label,
                                    CDiTResult graph) {
    graph = canonical_graph(std::move(graph));
    if (!segments->empty() && same_graph(segments->back().graph, graph)) {
        // 库外没有 C/B 元数据，因此只做确定性的相邻图合并：完整边集合及各边滞后集合完全相同才合并。
        segments->back().end_label = end_label;
        return;
    }
    segments->push_back({start_label, end_label, std::move(graph)});
}

static std::string format_graph(const CDiTResult& graph,
                                const ExperimentCase& c) {
    struct Item {
        int source = 0;
        int target = 0;
        int source_rank = 0;
        int target_rank = 0;
        std::vector<int> lags;
    };

    const int cause_count = static_cast<int>(c.cause_columns.size());
    const int result_count = static_cast<int>(c.result_columns.size());
    std::vector<Item> items;
    for (const auto& kv : graph.lags_by_edge) {
        if (kv.second.empty()) continue;
        const int source = kv.first.first;
        const int target = kv.first.second;
        // 即使底层实现未来改变，也只打印本实验允许的 原因列 -> 结果列。
        if (source < 1 || source > cause_count ||
            target <= cause_count || target > cause_count + result_count) {
            continue;
        }
        Item item;
        item.source = source;
        item.target = target;
        item.source_rank = source - 1;
        item.target_rank = target - cause_count - 1;
        item.lags = kv.second;
        std::sort(item.lags.begin(), item.lags.end());
        item.lags.erase(std::unique(item.lags.begin(), item.lags.end()), item.lags.end());
        items.push_back(std::move(item));
    }

    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.source_rank != b.source_rank) return a.source_rank < b.source_rank;
        if (a.target_rank != b.target_rank) return a.target_rank < b.target_rank;
        if (a.source != b.source) return a.source < b.source;
        return a.target < b.target;
    });

    std::ostringstream oss;
    for (const Item& item : items) {
        const std::string& source_name = c.cause_columns[static_cast<std::size_t>(item.source_rank)].alias;
        const std::string& target_name = c.result_columns[static_cast<std::size_t>(item.target_rank)].alias;
        oss << source_name << "→" << target_name << "滞后=";
        for (int lag : item.lags) oss << lag << ',';
        oss << "; ";
    }
    return oss.str();
}

static std::vector<InferenceSegment> infer_one_dataset_from_rocksdb(
        rocksdb::DB* db,
        const DatasetInfo& info,
        const ExperimentCase& c,
        QueryTiming* timing) {
    const std::vector<TimestampInfo> sampled = sampled_timestamps(info, c.sampling_step);
    const std::size_t T = static_cast<std::size_t>(c.window_length);
    const std::size_t complete_window_count = sampled.size() / T;

    std::vector<InferenceSegment> segments;
    segments.reserve(complete_window_count);
    const rocksdb::ReadOptions ropt;

    std::vector<int> source_columns;
    std::vector<int> target_columns;
    source_columns.reserve(c.cause_columns.size());
    target_columns.reserve(c.result_columns.size());
    for (std::size_t i = 0; i < c.cause_columns.size(); ++i) {
        source_columns.push_back(static_cast<int>(i) + 1);
    }
    for (std::size_t i = 0; i < c.result_columns.size(); ++i) {
        target_columns.push_back(static_cast<int>(c.cause_columns.size() + i) + 1);
    }

    const std::size_t expected_value_columns = c.cause_columns.size() + c.result_columns.size();
    for (std::size_t w = 0; w < complete_window_count; ++w) {
        const std::size_t begin = w * T;
        const std::size_t end = begin + T;
        std::vector<std::vector<double>> data;
        data.reserve(T);

        // 读取部分独立计时：每个完整窗口逐条通过 RocksDB 公共 DB::Get 读取持久化后的 value。
        const Clock::time_point read0 = Clock::now();
        for (std::size_t i = begin; i < end; ++i) {
            const TimestampInfo& ti = sampled[i];
            const std::string key = info.spec.dataset_id + "," + ti.key;
            std::string value;
            const rocksdb::Status s = db->Get(ropt, key, &value);
            if (!s.ok()) {
                throw std::runtime_error("RocksDB DB::Get 读取失败: key=" + key + " / " + s.ToString());
            }
            data.push_back(parse_db_value(value, expected_value_columns,
                                          info.spec.dataset_id, ti.key));
        }
        timing->read_seconds += seconds_between(read0, Clock::now());

        // 推断部分独立计时：只覆盖 CDiT/LSS-CDiT 本体，不用总时间做加减法推导。
        const Clock::time_point infer0 = Clock::now();
        CDiTResult graph = c.use_covmeta
                ? run_cdit_covmeta(data, c.tau, 0.01, false, &source_columns, &target_columns)
                : run_cdit_original(data, c.tau, 0.01, false, &source_columns, &target_columns);
        timing->inference_seconds += seconds_between(infer0, Clock::now());

        append_or_merge_segment(&segments,
                                sampled[begin].label,
                                sampled[end - 1].label,
                                std::move(graph));
    }
    return segments;
}

static void initialize_summary_csv(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("无法创建实验统计 CSV: " + path.string());
    // UTF-8 BOM，避免 Windows Excel 双击时按本地 ANSI/GBK 误判中文编码。
    out.write("\xEF\xBB\xBF", 3);
    out << "指定试验品集合,查询时间起止范围,参与的原因列,参与的结果列,采样步长,完整子随机过程长度T,推断时的τ,MemTable容量(MB),是否使用协方差缓存驱动CDiT,总耗时(s),插入总耗时(s),库外推断中仅读取部分耗时(s),库外推断中仅推断部分耗时(s)\n";
    out.flush();
}

static void append_summary_row(const fs::path& path,
                               const ExperimentCase& c,
                               const std::vector<DatasetSpec>& datasets,
                               double total_seconds,
                               double insert_seconds,
                               double read_seconds,
                               double inference_seconds) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("无法追加实验统计 CSV: " + path.string());

    const std::vector<std::string> fields = {
        join_strings(dataset_aliases(datasets), " "),
        excel_safe_time_range(c.time_range_text),
        join_column_aliases(c.cause_columns),
        join_column_aliases(c.result_columns),
        std::to_string(c.sampling_step),
        std::to_string(c.window_length),
        std::to_string(c.tau),
        std::to_string(c.memtable_mb),
        yesno(c.use_covmeta)
    };
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) out << ',';
        out << csv_escape(fields[i]);
    }
    out << ',' << std::fixed << std::setprecision(4) << total_seconds
        << ',' << std::fixed << std::setprecision(4) << insert_seconds
        << ',' << std::fixed << std::setprecision(4) << read_seconds
        << ',' << std::fixed << std::setprecision(4) << inference_seconds
        << '\n';
    // 一次实验结束立即写一行并 flush，运行过程中即可打开 CSV 查看已有结果。
    out.flush();
}

static QueryTiming run_outdb_inference(rocksdb::DB* db,
                                       const std::map<std::string, DatasetInfo>& infos,
                                       const std::vector<DatasetSpec>& datasets,
                                       const ExperimentCase& c,
                                       ExperimentLogger& logger) {
    QueryTiming timing;

    logger.line("本次范围推断输入：试验品=" + join_strings(dataset_aliases(datasets), ",") +
                "；因果查询起止=-∞到+∞；参与的原因列=" + join_column_aliases(c.cause_columns) +
                "；参与的结果列=" + join_column_aliases(c.result_columns) +
                "；采样步长=" + std::to_string(c.sampling_step) +
                "；T=" + std::to_string(c.window_length) +
                "；τ=" + std::to_string(c.tau) + "；");

    for (const DatasetSpec& spec : datasets) {
        logger.line("试验品：" + spec.dataset_id);
        const Clock::time_point one0 = Clock::now();
        const std::vector<InferenceSegment> segments =
                infer_one_dataset_from_rocksdb(db, infos.at(spec.dataset_id), c, &timing);
        timing.total_seconds += seconds_between(one0, Clock::now());
        for (const InferenceSegment& seg : segments) {
            logger.line(seg.start_label + "到" + seg.end_label + " | 推断：" +
                        format_graph(seg.graph, c));
        }
    }

    {
        std::ostringstream oss;
        oss << "本次范围查询小节：耗时:" << std::fixed << std::setprecision(4)
            << timing.total_seconds << "s，";
        logger.line(oss.str());
    }
    logger.line();
    return timing;
}

static rocksdb::Options make_options(const ExperimentCase& c) {
    rocksdb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = static_cast<std::size_t>(c.memtable_mb) * 1024ULL * 1024ULL;
    return options;
}

static void run_one_experiment(const ExperimentPaths& paths,
                               const std::map<std::string, fs::path>& discovered,
                               const ExperimentCase& c,
                               const std::vector<DatasetSpec>& datasets,
                               int experiment_index,
                               ExperimentLogger& logger) {
    rocksdb::Options options = make_options(c);

    // 第一次/每次实验正式计时前先恢复为空库；清库时间严格不进入任何实验耗时统计。
    destroy_db_state(paths.db_dir, options);

    // 总耗时从清库完成后的本次实验正式准备阶段开始；前后清库严格排除。
    const Clock::time_point total0 = Clock::now();

    // 本实验明确关闭 RocksDB 写路径上的 causal_meta 元数据维护。
    causal_meta_reset_all();
    causal_meta_set_enabled_cpp(false);
    std::string attach_error;
    if (!causal_meta_attach_to_rocksdb_options(&options, &attach_error)) {
        throw std::runtime_error(attach_error);
    }

    std::map<std::string, DatasetInfo> infos;
    for (const DatasetSpec& spec : datasets) {
        infos.emplace(spec.dataset_id, load_dataset_info(spec, discovered.at(spec.dataset_id), c));
    }

    logger.line("风洞实验第" + std::to_string(experiment_index) + "次实验");
    logger.line("本次数据库外纯CDiT推断+Merge实验参数设置：试验品=" +
                join_strings(dataset_aliases(datasets), ",") +
                "；因果查询起止=" + c.time_range_text +
                "；是否使用协方差缓存驱动CDiT：" + yesno(c.use_covmeta) + "；");
    logger.line();

    std::unique_ptr<rocksdb::DB> db;
    try {
        // 插入总耗时单独计时：从本次插入阶段启动数据库开始，到 Flush 后关闭插入实例为止。
        const Clock::time_point insert0 = Clock::now();
        rocksdb::Status s = rocksdb::DB::Open(options, paths.db_dir.string(), &db);
        if (!s.ok()) throw std::runtime_error("RocksDB Open 失败: " + s.ToString());

        for (const DatasetSpec& spec : datasets) {
            insert_dataset(db.get(), infos.at(spec.dataset_id), c);
        }

        // 显式同步 Flush，保证最后一个未达到容量阈值的 mutable MemTable 也刷成持久化 SST。
        rocksdb::FlushOptions fopt;
        fopt.wait = true;
        fopt.listener_wait = true;
        s = db->Flush(fopt);
        if (!s.ok()) throw std::runtime_error("RocksDB Flush 失败: " + s.ToString());

        // 关闭插入实例；后续重新 Open，确保库外推断只能从持久化后的 RocksDB 读取。
        db.reset();
        const double insert_seconds = seconds_between(insert0, Clock::now());

        rocksdb::Options read_options = options;
        read_options.create_if_missing = false;
        s = rocksdb::DB::Open(read_options, paths.db_dir.string(), &db);
        if (!s.ok()) throw std::runtime_error("RocksDB 重新打开失败: " + s.ToString());

        const QueryTiming query_timing = run_outdb_inference(db.get(), infos, datasets, c, logger);
        db.reset();

        const double total_seconds = seconds_between(total0, Clock::now());
        append_summary_row(paths.summary_csv, c, datasets, total_seconds, insert_seconds,
                           query_timing.read_seconds, query_timing.inference_seconds);
    } catch (...) {
        db.reset();
        destroy_db_state_noexcept(paths.db_dir, options);
        throw;
    }

    // 每次实验结束再次恢复为空库；同样位于总耗时与各分项耗时之外。
    destroy_db_state(paths.db_dir, options);
}

}  // 匿名命名空间结束

int run_outdb_windtunnel_experiments() {
    try {
        const ExperimentPaths paths = resolve_paths();
        if (paths.data_dir.empty()) {
            throw std::runtime_error(
                    "未找到风洞数据目录。请 export WINDTUNNEL_DATA_DIR=../remixdb/data/windtunnel_data");
        }

        const std::map<std::string, fs::path> discovered = discover_clean_data(paths.data_dir);
        const std::vector<ExperimentCase> cases = load_cases(paths.input_csv);

        ExperimentLogger logger(paths.print_log);
        logger.initialize();
        initialize_summary_csv(paths.summary_csv);

        int experiment_index = 0;
        for (const ExperimentCase& c : cases) {
            const std::vector<DatasetSpec> datasets = resolve_datasets(discovered, c);
            ++experiment_index;
            run_one_experiment(paths, discovered, c, datasets, experiment_index, logger);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "风洞入库+库外推断实验失败: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "风洞入库+库外推断实验失败: 未知异常" << std::endl;
        return 2;
    }
}
