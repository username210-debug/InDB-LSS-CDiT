/*
 * 本文件实现 WindTunnel 数据集在 RocksDB 中的实验流程，包括数据准备、写入、元数据维护、查询和统计。
 */

#include "experiment/windtunnel_experiment.h"

#include "causal_meta.h"
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
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

struct DatasetFiles {
    fs::path clean;
    std::map<std::string, fs::path> faults;
};

struct DiscoveredData {
    std::map<std::string, DatasetFiles> by_dataset_id;
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
    bool test_fault = false;
    bool print_process_log = false;
    bool use_covmeta = false;
};

struct DatasetBaseline {
    DatasetSpec spec;
    fs::path clean_path;
    std::vector<CausalMetaTimestampInfo> timestamps;
    std::unordered_set<std::string> allowed_timestamp_keys;
    long long expected_complete_windows = 0;
};

struct ExperimentPaths {
    fs::path root;
    fs::path data_dir;
    fs::path input_csv;
    fs::path meta_dir;
    fs::path db_dir;
    fs::path log_dir;
    fs::path summary_csv;
    fs::path print_log;
};

class ExperimentLogger {
public:
    explicit ExperimentLogger(fs::path print_log) : print_log_(std::move(print_log)) {}

    void initialize() {
        std::lock_guard<std::mutex> lk(mu_);
        fs::create_directories(print_log_.parent_path());
        log_stream_.close();
        log_stream_.clear();
        log_stream_.open(print_log_, std::ios::binary | std::ios::trunc);
        if (!log_stream_) {
            throw std::runtime_error("无法创建打印日志: " + print_log_.string());
        }
        // UTF-8 BOM：Windows/Excel/记事本双击时更稳定识别中文。
        log_stream_.write("\xEF\xBB\xBF", 3);
        log_stream_.flush();
    }

    void line(const std::string& text = std::string()) {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << text << '\n';
        if (log_stream_) {
            log_stream_ << text << '\n';
            // 每行立即刷新，长实验运行过程中可以直接打开日志查看已有结果。
            log_stream_.flush();
        }
    }

    static void causal_sink(const char* text, void* userdata) {
        if (!text || !userdata) return;
        static_cast<ExperimentLogger*>(userdata)->line(text);
    }

private:
    fs::path print_log_;
    std::ofstream log_stream_;
    std::mutex mu_;
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
    bool need = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            need = true;
            break;
        }
    }
    if (!need) return s;
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
    // parse_csv_line 已去掉 CSV 外层引号；这里处理 Excel 单元格公式 ="..."。
    if (s.size() >= 3 && s[0] == '=' && s[1] == '"' && s.back() == '"') {
        s = s.substr(2, s.size() - 3);
    }
    // 同时容忍用户手工编辑后残留的一层普通引号。
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return trim(s);
}

static std::string excel_safe_time_range(const std::string& s) {
    if (s == "-∞到+∞") return "=\"-∞到+∞\"";
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

static bool parse_positive_int(const std::string& raw, int* out) {
    const std::string s = normalize_excel_cell(raw);
    if (s.empty()) return false;
    std::size_t used = 0;
    try {
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
    std::size_t used = 0;
    try {
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
    std::ostringstream oss;
    oss << std::setprecision(17) << v;
    return oss.str();
}

static bool parse_time_range(const std::string& raw,
                             std::string* normalized,
                             std::optional<double>* start,
                             std::optional<double>* end,
                             std::string* error) {
    const std::string s = normalize_excel_cell(raw);
    const std::string low = ascii_lower(s);
    if (s == "-∞到+∞" || low == "-inf到+inf" || low == "-infinity到+infinity") {
        *normalized = "-∞到+∞";
        start->reset();
        end->reset();
        return true;
    }

    const std::string marker = "到";
    const std::size_t p = s.find(marker);
    if (p == std::string::npos) {
        if (error) *error = "无法解析时间范围: " + raw;
        return false;
    }
    double a = 0.0;
    double b = 0.0;
    if (!parse_finite_double(s.substr(0, p), &a) ||
        !parse_finite_double(s.substr(p + marker.size()), &b) || a > b) {
        if (error) *error = "时间范围非法: " + raw;
        return false;
    }
    *normalized = format_time_2(a) + "到" + format_time_2(b);
    *start = a;
    *end = b;
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
        throw std::runtime_error("第 " + std::to_string(line_no) +
                                 " 行的" + (want_cause ? "原因列" : "结果列") + "为空");
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

static const char* kFaultGuardVariableName = "__windtunnel_fault_guard__";

static std::vector<ColumnSpec> build_storage_columns(const ExperimentCase& c) {
    // 风洞实验的推断 value 只保存“参与因果发现”的原因列 + 结果列。
    // 这是本次修复的关键：run_cdit_covmeta 会按传入矩阵的总维数建立滞后协方差缓存；
    // 若为了故障判断把未参与推断的 21 个业务列也全部塞入 value，缓存预计算会为无关列付出
    // O((d*2τ)^2*T) 的代价，掩盖协方差缓存驱动 CDiT 的优势。
    std::vector<ColumnSpec> out;
    out.reserve(c.cause_columns.size() + c.result_columns.size());
    std::unordered_set<std::string> seen;
    for (const ColumnSpec& x : c.cause_columns) {
        if (seen.insert(x.alias).second) out.push_back(x);
    }
    for (const ColumnSpec& x : c.result_columns) {
        if (seen.insert(x.alias).second) out.push_back(x);
    }
    return out;
}

static bool needs_fault_guard(const std::vector<ColumnSpec>& storage_columns) {
    // 如果本次恰好选择了全部 21 个业务列，causal_meta 可直接用这些值判断“全零故障”，
    // 无需额外维度；否则追加一个仅用于故障判定、绝不参与 source/target 推断的内部 guard。
    return storage_columns.size() != all_columns().size();
}

static std::vector<std::string> selected_column_aliases(const ExperimentCase& c) {
    std::vector<std::string> out;
    out.reserve(c.cause_columns.size() + c.result_columns.size());
    for (const ColumnSpec& x : c.cause_columns) out.push_back(x.alias);
    for (const ColumnSpec& x : c.result_columns) out.push_back(x.alias);
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
    p.input_csv = p.root / "experiment" / "windtunnel_experiment_input.csv";
    p.meta_dir = p.root / "meta_file" / "windtunnel_experiment";
    p.db_dir = p.root / "rdb_windtunnel_experiment";
    p.log_dir = p.root / "experiment_log";
    p.summary_csv = p.log_dir / "风洞实验全部实验小结.csv";
    p.print_log = p.log_dir / "风洞实验全部实验打印日志.txt";

    if (const char* env = std::getenv("WINDTUNNEL_DATA_DIR")) {
        if (*env) p.data_dir = fs::path(env);
    }
    if (const char* env = std::getenv("WINDTUNNEL_EXPERIMENT_INPUT")) {
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

static DiscoveredData discover_data(const fs::path& root) {
    if (root.empty() || !fs::is_directory(root)) {
        throw std::runtime_error("找不到 windtunnel_data 目录；可设置 WINDTUNNEL_DATA_DIR。当前目录: " + root.string());
    }

    DiscoveredData d;
    for (const auto& ent : fs::recursive_directory_iterator(root)) {
        if (!ent.is_regular_file()) continue;
        const std::string name = ent.path().filename().string();
        if (name.size() < 8 || name.substr(name.size() - 4) != ".csv") continue;
        const std::size_t pre = name.rfind("_pre");
        if (pre == std::string::npos) continue;
        const std::string dataset_id = name.substr(0, pre);
        if (dataset_id.empty()) continue;
        const std::string tail = name.substr(pre + 4);  // ".csv" 或 "_severity.csv"。
        DatasetFiles& files = d.by_dataset_id[dataset_id];
        if (tail == ".csv") {
            files.clean = ent.path();
        } else if (tail.size() > 5 && tail.front() == '_' && tail.substr(tail.size() - 4) == ".csv") {
            const std::string severity = tail.substr(1, tail.size() - 5);
            if (!severity.empty()) files.faults[severity] = ent.path();
        }
    }

    bool have_clean = false;
    for (const auto& kv : d.by_dataset_id) {
        if (!kv.second.clean.empty()) {
            have_clean = true;
            break;
        }
    }
    if (!have_clean) throw std::runtime_error("windtunnel_data 中没有发现 *_pre.csv");
    return d;
}

static std::vector<ExperimentCase> load_cases(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开实验输入文件: " + path.string());
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("实验输入文件为空");
    line = remove_utf8_bom(line);

    std::vector<ExperimentCase> out;
    int line_no = 1;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        const std::vector<std::string> f = parse_csv_line(line);
        if (f.size() < 11) {
            throw std::runtime_error("windtunnel_experiment_input.csv 第 " +
                                     std::to_string(line_no) + " 行少于 11 列");
        }

        ExperimentCase c;
        c.requested_dataset_tokens = split_list_field(f[0]);
        if (c.requested_dataset_tokens.empty()) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行指定试验品集合为空");
        }

        std::string err;
        if (!parse_time_range(f[1], &c.time_range_text, &c.start_time, &c.end_time, &err)) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行: " + err);
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
        if (!parse_yes_no(f[8], &c.test_fault) ||
            !parse_yes_no(f[9], &c.print_process_log) ||
            !parse_yes_no(f[10], &c.use_covmeta)) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行的是/否字段无法识别");
        }
        out.push_back(std::move(c));
    }
    if (out.empty()) throw std::runtime_error("实验输入文件没有有效实验行");
    return out;
}

static std::vector<DatasetSpec> resolve_datasets(const DiscoveredData& d,
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
            auto it = d.by_dataset_id.find(s.dataset_id);
            if (it != d.by_dataset_id.end() && !it->second.clean.empty()) {
                out.push_back(s);
                seen.insert(s.dataset_id);
            }
        }
    } else {
        for (const std::string& token0 : c.requested_dataset_tokens) {
            const std::string token = ascii_lower(trim(token0));
            DatasetSpec spec;
            bool found = false;
            auto a = by_alias.find(token);
            if (a != by_alias.end()) {
                spec = a->second;
                found = true;
            } else {
                auto b = by_id.find(token);
                if (b != by_id.end()) {
                    spec = b->second;
                    found = true;
                }
            }
            if (!found) throw std::runtime_error("未知风洞试验品简写/数据集名: " + token0);
            auto fit = d.by_dataset_id.find(spec.dataset_id);
            if (fit == d.by_dataset_id.end() || fit->second.clean.empty()) {
                throw std::runtime_error("指定试验品缺少 clean 文件: " + spec.alias +
                                         " (" + spec.dataset_id + "_pre.csv)");
            }
            if (seen.insert(spec.dataset_id).second) out.push_back(spec);
        }
    }
    if (out.empty()) throw std::runtime_error("本次实验没有可用风洞试验品");
    return out;
}

static int severity_percent(const std::string& s) {
    const std::size_t p = s.find('%');
    if (p == std::string::npos || p == 0) return std::numeric_limits<int>::max();
    try {
        std::size_t used = 0;
        const int v = std::stoi(s.substr(0, p), &used);
        if (used == p) return v;
    } catch (...) {
    }
    return std::numeric_limits<int>::max();
}

static std::vector<std::string> severities_for(const DiscoveredData& d,
                                               const std::vector<DatasetSpec>& datasets,
                                               bool test_fault) {
    std::vector<std::string> out = {""};  // 空串表示无故障 clean。
    if (!test_fault || datasets.empty()) return out;

    std::set<std::string> common;
    bool first = true;
    for (const DatasetSpec& spec : datasets) {
        std::set<std::string> here;
        const auto it = d.by_dataset_id.find(spec.dataset_id);
        if (it != d.by_dataset_id.end()) {
            for (const auto& kv : it->second.faults) here.insert(kv.first);
        }
        if (first) {
            common = std::move(here);
            first = false;
        } else {
            std::set<std::string> next;
            std::set_intersection(common.begin(), common.end(), here.begin(), here.end(),
                                  std::inserter(next, next.begin()));
            common.swap(next);
        }
    }

    std::vector<std::string> ordered(common.begin(), common.end());
    std::sort(ordered.begin(), ordered.end(), [](const std::string& a, const std::string& b) {
        const int pa = severity_percent(a);
        const int pb = severity_percent(b);
        if (pa != pb) return pa < pb;
        return a < b;
    });
    out.insert(out.end(), ordered.begin(), ordered.end());
    return out;
}

static fs::path choose_data_file(const DiscoveredData& d,
                                 const DatasetSpec& spec,
                                 const std::string& severity) {
    const auto it = d.by_dataset_id.find(spec.dataset_id);
    if (it == d.by_dataset_id.end()) {
        throw std::runtime_error("内部错误：未发现数据集 " + spec.dataset_id);
    }
    if (severity.empty()) return it->second.clean;
    const auto fit = it->second.faults.find(severity);
    if (fit == it->second.faults.end()) {
        throw std::runtime_error("缺少故障数据文件: " + spec.dataset_id + " / " + severity);
    }
    return fit->second;
}

static std::map<std::string, int> header_index(const std::vector<std::string>& header) {
    std::map<std::string, int> out;
    for (std::size_t i = 0; i < header.size(); ++i) {
        out[trim(header[i])] = static_cast<int>(i);
    }
    return out;
}

static DatasetBaseline load_baseline(const DiscoveredData& d,
                                     const DatasetSpec& spec,
                                     const ExperimentCase& c) {
    const fs::path path = d.by_dataset_id.at(spec.dataset_id).clean;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开 clean 数据集: " + path.string());

    DatasetBaseline b;
    b.spec = spec;
    b.clean_path = path;

    std::string line;
    // 空文件属于“本次没有要插入的内容”，不是实验失败。
    // 返回空 baseline，调用方会跳过注册/插入/查询，但仍正常完成本次实验并写小结。
    if (!std::getline(in, line)) return b;
    const std::map<std::string, int> idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    if (!idx.count("timestamp")) {
        throw std::runtime_error("数据集缺少 timestamp 列: " + path.string());
    }
    for (const ColumnSpec& col : all_columns()) {
        if (!idx.count(col.csv_name)) {
            throw std::runtime_error("数据集缺少列 " + col.csv_name + ": " + path.string());
        }
    }

    const int time_col = idx.at("timestamp");
    bool have_last = false;
    double last_ts = 0.0;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::vector<std::string> f = parse_csv_line(line);
        if (time_col >= static_cast<int>(f.size())) continue;
        double ts = 0.0;
        if (!parse_finite_double(f[static_cast<std::size_t>(time_col)], &ts)) {
            throw std::runtime_error("无法解析 timestamp: " + f[static_cast<std::size_t>(time_col)] +
                                     " / " + path.string());
        }
        if (!time_in_range(ts, c.start_time, c.end_time)) continue;
        if (have_last && !(ts > last_ts)) {
            throw std::runtime_error("clean 数据集时间戳不是严格递增: " + spec.dataset_id +
                                     " / " + format_time_2(ts));
        }
        have_last = true;
        last_ts = ts;
        const std::string key = canonical_timestamp_key(ts);
        if (!b.allowed_timestamp_keys.insert(key).second) {
            throw std::runtime_error("clean 数据集出现重复时间戳: " + spec.dataset_id + " / " + key);
        }
        b.timestamps.push_back({key, format_time_2(ts)});
    }

    // 指定时间范围与该数据集没有交集时，同样视为合法的“空贡献”。
    // 不在这里抛异常；register_datasets() 会跳过空 baseline。
    const long long sampled =
            (static_cast<long long>(b.timestamps.size()) + c.sampling_step - 1) / c.sampling_step;
    b.expected_complete_windows = sampled / c.window_length;
    return b;
}

static void insert_dataset(rocksdb::DB* db,
                           const fs::path& path,
                           const DatasetBaseline& baseline,
                           const std::vector<ColumnSpec>& storage_columns) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开插入文件: " + path.string());
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("插入文件为空: " + path.string());
    const std::map<std::string, int> idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    if (!idx.count("timestamp")) {
        throw std::runtime_error("插入文件缺少 timestamp 列: " + path.string());
    }

    std::vector<int> value_cols;
    value_cols.reserve(storage_columns.size());
    int max_col = idx.at("timestamp");
    for (const ColumnSpec& col : storage_columns) {
        auto it = idx.find(col.csv_name);
        if (it == idx.end()) {
            throw std::runtime_error("插入文件缺少列 " + col.csv_name + ": " + path.string());
        }
        value_cols.push_back(it->second);
        max_col = std::max(max_col, it->second);
    }

    const bool append_fault_guard = needs_fault_guard(storage_columns);
    std::vector<int> fault_check_cols;
    if (append_fault_guard) {
        fault_check_cols.reserve(all_columns().size());
        for (const ColumnSpec& col : all_columns()) {
            auto it = idx.find(col.csv_name);
            if (it == idx.end()) {
                throw std::runtime_error("插入文件缺少故障判定列 " + col.csv_name + ": " +
                                         path.string());
            }
            fault_check_cols.push_back(it->second);
            max_col = std::max(max_col, it->second);
        }
    }

    rocksdb::WriteOptions wopt;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::vector<std::string> f = parse_csv_line(line);
        if (max_col >= static_cast<int>(f.size())) {
            throw std::runtime_error("插入文件存在列数不足的数据行: " + path.string());
        }
        double ts = 0.0;
        if (!parse_finite_double(f[static_cast<std::size_t>(idx.at("timestamp"))], &ts)) {
            throw std::runtime_error("插入文件存在非法 timestamp: " + path.string());
        }
        const std::string ts_key = canonical_timestamp_key(ts);
        if (!baseline.allowed_timestamp_keys.count(ts_key)) continue;

        std::ostringstream value;
        for (std::size_t i = 0; i < value_cols.size(); ++i) {
            if (i) value << ',';
            const std::string cell = trim(f[static_cast<std::size_t>(value_cols[i])]);
            value << (cell.empty() ? "0" : cell);
        }

        if (append_fault_guard) {
            // guard=0 当且仅当原始 21 个业务变量全部为 0；否则 guard=1。
            // causal_meta 仍沿用其通用的“整行 value 全零即故障”判断，因此无需修改通用模块。
            bool all_business_zero = true;
            for (int col : fault_check_cols) {
                const std::string cell = trim(f[static_cast<std::size_t>(col)]);
                double v = 0.0;
                if (!cell.empty() && !parse_finite_double(cell, &v)) {
                    throw std::runtime_error("插入文件存在非法数值: " + cell + " / " + path.string());
                }
                if (std::fabs(v) > 1e-12) {
                    all_business_zero = false;
                    break;
                }
            }
            value << ',' << (all_business_zero ? '0' : '1');
        }

        const std::string key = baseline.spec.dataset_id + "," + ts_key;
        const rocksdb::Status s = db->Put(wopt, key, value.str());
        if (!s.ok()) throw std::runtime_error("RocksDB Put 失败: " + s.ToString());
    }
}

static void destroy_db_state(const fs::path& db_dir, const rocksdb::Options& options) {
    // RocksDB 官方公开接口 DestroyDB 会删除该 DB 的 SST/WAL/MANIFEST/OPTIONS 等状态。
    const rocksdb::Status s = rocksdb::DestroyDB(db_dir.string(), options);
    if (!s.ok() && !s.IsNotFound()) {
        throw std::runtime_error("DestroyDB 失败: " + s.ToString());
    }
    // 额外处理异常强退留下、但不属于 RocksDB 标准文件集合的残留。
    std::error_code ec;
    fs::remove_all(db_dir, ec);
}

static std::string yesno(bool v) { return v ? "是" : "否"; }

static std::vector<std::string> dataset_aliases(const std::vector<DatasetSpec>& datasets) {
    std::vector<std::string> out;
    out.reserve(datasets.size());
    for (const DatasetSpec& s : datasets) out.push_back(s.alias);
    return out;
}

static void initialize_summary_csv(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("无法创建实验汇总 CSV: " + path.string());
    out.write("\xEF\xBB\xBF", 3);
    out << "插入的时间范围,插入的试验品集合,参与插入的原因列,参与插入的结果列,采样步长,插入时的T,推断时的τ,MemTable(MB),是否测试故障数据集,是否打印过程日志,是否使用协方差缓存驱动CDiT,插入耗时(s),全数据集全跨度全列范围查询总耗时(s),库内推断算法部分耗时(s),故障程度,全部页面数,互补页面数,重叠页面数,插入总耗时减库内推断耗时(s)\n";
    out.flush();
}

static void append_summary_row(const fs::path& path,
                               const ExperimentCase& c,
                               const std::vector<DatasetSpec>& datasets,
                               double insert_seconds,
                               double query_seconds,
                               const CausalMetaCounters& counters,
                               const std::string& severity) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("无法追加风洞实验汇总 CSV");

    const std::vector<std::string> fields = {
        excel_safe_time_range(c.time_range_text),
        join_strings(dataset_aliases(datasets), ","),
        join_column_aliases(c.cause_columns),
        join_column_aliases(c.result_columns),
        std::to_string(c.sampling_step),
        std::to_string(c.window_length),
        std::to_string(c.tau),
        std::to_string(c.memtable_mb),
        yesno(c.test_fault),
        yesno(c.print_process_log),
        yesno(c.use_covmeta)
    };
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) out << ',';
        out << csv_escape(fields[i]);
    }
    const double derived = insert_seconds - counters.metadata_seconds;
    out << ',' << std::fixed << std::setprecision(6) << insert_seconds
        << ',' << std::fixed << std::setprecision(6) << query_seconds
        << ',' << std::fixed << std::setprecision(6) << counters.metadata_seconds
        << ',' << csv_escape(severity.empty() ? "无" : severity)
        << ',' << counters.total_pages
        << ',' << counters.complementary_pages
        << ',' << counters.overlapped_pages
        << ',' << std::fixed << std::setprecision(6) << derived
        << '\n';
    out.flush();
}

static std::string format_query_segment(const CausalMetaQuerySegment& seg,
                                        const std::vector<std::string>& preferred_order) {
    std::map<std::string, int> rank;
    for (std::size_t i = 0; i < preferred_order.size(); ++i) {
        rank[preferred_order[i]] = static_cast<int>(i);
    }
    auto name_of = [&](int idx) {
        if (idx >= 1 && idx <= static_cast<int>(seg.variable_names.size())) {
            return seg.variable_names[static_cast<std::size_t>(idx - 1)];
        }
        return std::string("X") + std::to_string(idx);
    };
    struct Item {
        int src_rank = 0;
        int dst_rank = 0;
        int src = 0;
        int dst = 0;
        std::string src_name;
        std::string dst_name;
        std::vector<int> lags;
    };
    std::vector<Item> items;
    for (const auto& kv : seg.graph.lags_by_edge) {
        if (kv.second.empty()) continue;
        const std::string s = name_of(kv.first.first);
        const std::string t = name_of(kv.first.second);
        const int sr = rank.count(s) ? rank[s] : 100000 + kv.first.first;
        const int tr = rank.count(t) ? rank[t] : 100000 + kv.first.second;
        items.push_back({sr, tr, kv.first.first, kv.first.second, s, t, kv.second});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.src_rank != b.src_rank) return a.src_rank < b.src_rank;
        if (a.dst_rank != b.dst_rank) return a.dst_rank < b.dst_rank;
        if (a.src != b.src) return a.src < b.src;
        return a.dst < b.dst;
    });

    std::ostringstream oss;
    oss << seg.start_label << "到" << seg.end_label << " | 推断：";
    for (Item item : items) {
        std::sort(item.lags.begin(), item.lags.end());
        item.lags.erase(std::unique(item.lags.begin(), item.lags.end()), item.lags.end());
        oss << item.src_name << "→" << item.dst_name << "滞后=";
        for (int lag : item.lags) oss << lag << ',';
        oss << "; ";
    }
    return oss.str();
}

static double run_queries(const ExperimentPaths& paths,
                          const std::vector<DatasetBaseline>& baselines,
                          const ExperimentCase& c,
                          ExperimentLogger& logger) {
    const std::vector<std::string> query_columns = selected_column_aliases(c);
    double total_query_seconds = 0.0;
    for (const DatasetBaseline& b : baselines) {
        logger.line("本次范围查询输入：查询的试验品: " + b.spec.dataset_id +
                    "；查询时间区间:-∞到+∞；查询列:" + join_strings(query_columns, ","));
        logger.line("查询结果如下");

        const Clock::time_point t0 = Clock::now();
        std::string err;
        const auto segs = causal_meta_query_dataset_from_file(
                paths.meta_dir.string(), b.spec.dataset_id, "", "", query_columns, &err);
        if (!err.empty()) throw std::runtime_error(err);
        std::vector<std::string> result_lines;
        result_lines.reserve(segs.size());
        for (const auto& seg : segs) result_lines.push_back(format_query_segment(seg, query_columns));
        const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        total_query_seconds += sec;

        // 终端/日志文件 I/O 不计入查询算法耗时；计时覆盖读 C、投影、排序和结果字符串组织。
        for (const std::string& x : result_lines) logger.line(x);
        std::ostringstream oss;
        oss << "本次范围查询小节：耗时:" << std::fixed << std::setprecision(6) << sec << 's';
        logger.line(oss.str());
        logger.line();
    }
    return total_query_seconds;
}

static std::vector<DatasetBaseline> register_datasets(const DiscoveredData& d,
                                                      const ExperimentCase& c,
                                                      const std::vector<DatasetSpec>& datasets,
                                                      const std::vector<ColumnSpec>& storage_columns) {
    std::vector<DatasetBaseline> baselines;
    baselines.reserve(datasets.size());

    std::vector<std::string> variable_names;
    variable_names.reserve(storage_columns.size() + 1);
    for (const ColumnSpec& col : storage_columns) variable_names.push_back(col.alias);
    if (needs_fault_guard(storage_columns)) {
        variable_names.push_back(kFaultGuardVariableName);
    }

    std::vector<int> source_columns;
    std::vector<int> target_columns;
    for (std::size_t i = 0; i < c.cause_columns.size(); ++i) {
        source_columns.push_back(static_cast<int>(i) + 1);
    }
    for (std::size_t i = 0; i < c.result_columns.size(); ++i) {
        target_columns.push_back(static_cast<int>(c.cause_columns.size() + i) + 1);
    }

    for (const DatasetSpec& spec : datasets) {
        DatasetBaseline b = load_baseline(d, spec, c);
        // causal_meta 的通用接口要求 ordered_timestamps 非空。
        // 本实验层在这里过滤“空文件/时间范围无交集”的试验品，避免把数据集特定的
        // 空输入语义写进 causal_meta 通用模块。未过滤掉的试验品才真正入库、维护 C/B 并参与查询。
        if (b.timestamps.empty()) continue;

        CausalMetaDatasetConfig cfg;
        cfg.dataset_id = spec.dataset_id;
        cfg.display_name = spec.alias;  // 页面过程日志只显示 w1/w2/... 等简写。
        cfg.variable_names = variable_names;
        cfg.ordered_timestamps = b.timestamps;
        cfg.sampling_step = c.sampling_step;
        cfg.window_length = c.window_length;
        cfg.tau = c.tau;
        cfg.alpha = 0.01;
        cfg.infer_self = false;  // 本实验明确忽略自连接。
        cfg.use_covariance_cache_cdit = c.use_covmeta;
        cfg.source_columns = source_columns;
        cfg.target_columns = target_columns;
        std::string err;
        if (!causal_meta_register_dataset(cfg, &err)) throw std::runtime_error(err);
        baselines.push_back(std::move(b));
    }
    return baselines;
}

static double compression_ratio_percent(const std::vector<DatasetBaseline>& baselines,
                                        const CausalMetaCounters& counters) {
    long long expected = 0;
    for (const DatasetBaseline& b : baselines) expected += b.expected_complete_windows;
    if (expected <= 0) return 0.0;
    if (counters.final_metadata_entries > expected) {
        throw std::runtime_error("元数据条目数超过按采样步长/T计算出的完整窗口数，无法计算合并压缩率");
    }
    return 100.0 * static_cast<double>(expected - counters.final_metadata_entries) /
           static_cast<double>(expected);
}

static void run_one(const ExperimentPaths& paths,
                    const DiscoveredData& discovered,
                    const ExperimentCase& c,
                    const std::vector<DatasetSpec>& datasets,
                    const std::string& severity,
                    int experiment_index,
                    ExperimentLogger& logger) {
    // 每次实验开始前清空元数据目录；该清理时间不计入任何实验耗时。
    std::error_code meta_ec;
    fs::remove_all(paths.meta_dir, meta_ec);
    fs::create_directories(paths.meta_dir);

    causal_meta_reset_all();
    causal_meta_set_enabled_cpp(true);  // 本次库内实验默认开启元数据维护。
    causal_meta_set_metadata_dir(paths.meta_dir.string());
    causal_meta_set_page_logging(c.print_process_log);
    causal_meta_set_log_sink(&ExperimentLogger::causal_sink, &logger);

    const std::vector<ColumnSpec> storage_columns = build_storage_columns(c);
    const std::vector<DatasetBaseline> baselines =
            register_datasets(discovered, c, datasets, storage_columns);

    rocksdb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = static_cast<std::size_t>(c.memtable_mb) * 1024ULL * 1024ULL;
    // 一个 active + 一个 immutable，并让每个 immutable 单独 Flush。
    // 因此 causal_meta 的一次成功 Flush 捕获与“Immutable MemTable 页面”一一对应。
    options.max_write_buffer_number = 2;
    options.min_write_buffer_number_to_merge = 1;
    options.max_background_flushes = 1;
    options.max_background_compactions = 1;

    std::string attach_error;
    if (!causal_meta_attach_to_rocksdb_options(&options, &attach_error)) {
        throw std::runtime_error(attach_error);
    }

    // 第一次实验前以及每次实验开始前都调用 RocksDB 官方 DestroyDB，清除异常强退遗留状态。
    destroy_db_state(paths.db_dir, options);

    logger.line("第" + std::to_string(experiment_index) + "次实验");
    logger.line("数据库参数设置：MemTable=" + std::to_string(c.memtable_mb) + "MB");
    logger.line("本此插入实验参数设置：要插入的试验品=" +
                join_strings(dataset_aliases(datasets), ",") +
                "；插入时间范围=" + c.time_range_text +
                "；原因列=" + join_column_aliases(c.cause_columns) +
                "；结果列=" + join_column_aliases(c.result_columns) +
                "；采样步长=" + std::to_string(c.sampling_step) +
                "；T=" + std::to_string(c.window_length) +
                "；τ=" + std::to_string(c.tau) +
                "；是否使用协方差缓存驱动CDiT：" + yesno(c.use_covmeta) +
                "；故障程度=" + (severity.empty() ? "无" : severity) + "；");
    logger.line();

    std::unique_ptr<rocksdb::DB> db;
    const rocksdb::Status os = rocksdb::DB::Open(options, paths.db_dir.string(), &db);
    if (!os.ok()) {
        causal_meta_clear_log_sink();
        throw std::runtime_error("RocksDB Open 失败: " + os.ToString());
    }

    try {
        const Clock::time_point insert0 = Clock::now();
        for (const DatasetBaseline& b : baselines) {
            insert_dataset(db.get(),
                           choose_data_file(discovered, b.spec, severity),
                           b,
                           storage_columns);
        }

        // 最后一个未达到容量阈值的 mutable MemTable 也必须成为本次实验的一页。
        rocksdb::FlushOptions fopt;
        fopt.wait = true;
        fopt.listener_wait = true;
        const rocksdb::Status fs_status = db->Flush(fopt);
        if (!fs_status.ok()) throw std::runtime_error("RocksDB Flush 失败: " + fs_status.ToString());

        causal_meta_wait_for_idle();
        std::string persist_err;
        if (!causal_meta_persist_all(&persist_err)) throw std::runtime_error(persist_err);

        const double insert_seconds = std::chrono::duration<double>(Clock::now() - insert0).count();
        const CausalMetaCounters counters = causal_meta_get_counters();
        const double compression = compression_ratio_percent(baselines, counters);

        {
            std::ostringstream oss;
            oss << "本此插入完毕 | 耗时：" << std::fixed << std::setprecision(6)
                << insert_seconds << "s；合并压缩率：" << std::fixed << std::setprecision(2)
                << compression << '%';
            logger.line(oss.str());
        }
        logger.line();

        const double query_seconds = run_queries(paths, baselines, c, logger);
        append_summary_row(paths.summary_csv, c, datasets, insert_seconds,
                           query_seconds, counters, severity);

        // 关闭 RocksDB 前再次执行 barrier + C/B 持久化；无新脏页时是快速 no-op。
        std::string close_err;
        if (!causal_meta_before_db_close(&close_err)) throw std::runtime_error(close_err);
        db.reset();
        causal_meta_clear_log_sink();

        // 每次实验结束后恢复为空库；该时间严格位于实验耗时统计之外。
        destroy_db_state(paths.db_dir, options);
    } catch (...) {
        std::string ignored;
        causal_meta_before_db_close(&ignored);
        db.reset();
        causal_meta_clear_log_sink();
        try {
            destroy_db_state(paths.db_dir, options);
        } catch (...) {
        }
        throw;
    }
}

}  // 匿名命名空间结束

int run_windtunnel_experiments() {
    try {
        const ExperimentPaths paths = resolve_paths();
        if (paths.data_dir.empty()) {
            throw std::runtime_error(
                    "未找到风洞数据目录。请 export WINDTUNNEL_DATA_DIR=../remixdb/data/windtunnel_data");
        }

        const DiscoveredData discovered = discover_data(paths.data_dir);
        const std::vector<ExperimentCase> cases = load_cases(paths.input_csv);

        ExperimentLogger logger(paths.print_log);
        logger.initialize();
        initialize_summary_csv(paths.summary_csv);

        int experiment_index = 0;
        for (std::size_t case_index = 0; case_index < cases.size(); ++case_index) {
            const ExperimentCase& c = cases[case_index];
            const std::vector<DatasetSpec> datasets = resolve_datasets(discovered, c);
            const std::vector<std::string> severities = severities_for(discovered, datasets, c.test_fault);
            if (c.test_fault && severities.size() == 1) {
                logger.line("警告：实验输入第" + std::to_string(case_index + 2) +
                            "行要求故障数据，但所选试验品没有共同的故障程度；只执行无故障版本。");
            }
            for (const std::string& severity : severities) {
                ++experiment_index;
                run_one(paths, discovered, c, datasets, severity, experiment_index, logger);
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "风洞实验失败: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "风洞实验失败: 未知异常" << std::endl;
        return 2;
    }
}
