/*
 * 本文件实现 Basin 数据集在 RocksDB 中的实验流程，包括数据准备、写入、元数据维护、查询和统计。
 */

#include "experiment/basin_experiment.h"

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

struct ExperimentCase {
    bool target_is_count = false;
    int target_count = 0;
    std::vector<std::string> target_ids;
    std::string input_target_field;

    std::string time_range_text;
    std::optional<int> start_date;
    std::optional<int> end_date;

    int sampling_step = 1;
    int window_length = 365;
    int tau = 5;
    int memtable_mb = 2;
    bool test_fault = false;
    bool print_process_log = false;
    bool use_covmeta = false;
};

struct DatasetBaseline {
    std::string id;
    fs::path clean_path;
    std::vector<CausalMetaTimestampInfo> timestamps;
    std::unordered_set<std::string> allowed_timestamp_keys;
};

struct DiscoveredData {
    std::map<std::string, fs::path> clean;
    std::map<std::string, std::map<std::string, fs::path>> fault;
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
        if (!log_stream_) throw std::runtime_error("无法创建打印日志: " + print_log_.string());
        // UTF-8 BOM，Excel/记事本双击时更稳定识别中文。
        log_stream_.write("\xEF\xBB\xBF", 3);
        log_stream_.flush();
    }

    void line(const std::string& text = std::string()) {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << text << '\n';
        if (log_stream_) {
            log_stream_ << text << '\n';
            // 每行刷新：仍能在长实验运行中实时查看历史日志，但避免每行反复 open/close。
            log_stream_.flush();
        }
    }

    static void causal_sink(const char* text, void* userdata) {
        if (!userdata || !text) return;
        static_cast<ExperimentLogger*>(userdata)->line(text);
    }

private:
    fs::path print_log_;
    std::ofstream log_stream_;
    std::mutex mu_;
};

static std::string trim(const std::string& s) {
    std::size_t l = 0, r = s.size();
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

// RFC4180 风格的一行 CSV 解析；支持 Excel 的双引号转义。
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
            if (c == '"') quoted = true;
            else if (c == ',') {
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
    for (char c : s) if (c == ',' || c == '"' || c == '\n' || c == '\r') need = true;
    if (!need) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

static std::string excel_safe_time_range(const std::string& s) {
    if (s == "-∞到+∞") return "=\"-∞到+∞\"";
    return s;
}

static std::string normalize_excel_cell(std::string s) {
    s = trim(s);
    // parse_csv_line 已去掉 CSV 外层引号；此处处理 Excel 单元格公式 ="..."。
    if (s.size() >= 3 && s[0] == '=' && s[1] == '"' && s.back() == '"') {
        s = s.substr(2, s.size() - 3);
    }
    return trim(s);
}

static bool parse_yes_no(const std::string& raw, bool* out) {
    std::string s = normalize_excel_cell(raw);
    std::string low;
    low.reserve(s.size());
    for (unsigned char c : s) low.push_back(static_cast<char>(std::tolower(c)));
    if (s == "是" || low == "yes" || low == "true" || s == "1") { *out = true; return true; }
    if (s == "否" || low == "no" || low == "false" || s == "0") { *out = false; return true; }
    return false;
}

static bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    return true;
}

static int parse_basin_date(const std::string& raw) {
    std::string s = trim(raw);
    int y = 0, m = 0, d = 0;
    char sep1 = 0, sep2 = 0;
    std::istringstream iss(s);
    if (!(iss >> y >> sep1 >> m >> sep2 >> d) ||
        (sep1 != '/' && sep1 != '-') || (sep2 != '/' && sep2 != '-')) {
        return -1;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
    return y * 10000 + m * 100 + d;
}

static bool parse_time_range(const std::string& raw,
                             std::string* normalized,
                             std::optional<int>* start,
                             std::optional<int>* end,
                             std::string* error) {
    std::string s = normalize_excel_cell(raw);
    if (s == "-∞到+∞" || s == "-inf到+inf" || s == "-INF到+INF") {
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
    const std::string a = trim(s.substr(0, p));
    const std::string b = trim(s.substr(p + marker.size()));
    const int da = parse_basin_date(a);
    const int db = parse_basin_date(b);
    if (da < 0 || db < 0 || da > db) {
        if (error) *error = "时间范围非法: " + raw;
        return false;
    }
    *normalized = a + "到" + b;
    *start = da;
    *end = db;
    return true;
}

static bool date_in_range(const std::string& ts,
                          const std::optional<int>& start,
                          const std::optional<int>& end) {
    if (!start && !end) return true;
    const int d = parse_basin_date(ts);
    if (d < 0) return false;
    if (start && d < *start) return false;
    if (end && d > *end) return false;
    return true;
}

static ExperimentPaths resolve_paths() {
    ExperimentPaths p;
    p.root = fs::current_path();
    p.input_csv = p.root / "experiment" / "basin_experiment_input.csv";
    p.meta_dir = p.root / "meta_file" / "basin_experiment";
    p.db_dir = p.root / "rdb_basin_experiment";
    p.log_dir = p.root / "experiment_log";
    p.summary_csv = p.log_dir / "河川径流实验全部小节.csv";
    p.print_log = p.log_dir / "河川径流实验全部打印日志.txt";

    if (const char* env = std::getenv("BASIN_DATA_DIR")) {
        if (*env) p.data_dir = fs::path(env);
    }
    if (p.data_dir.empty()) {
        const std::vector<fs::path> candidates = {
            p.root / ".." / "remixdb" / "data" / "basin_data",
            p.root / "data" / "basin_data",
            p.root / ".." / "remixdb" / "data" / "basin_data"
        };
        for (const auto& c : candidates) {
            std::error_code ec;
            if (fs::is_directory(c, ec)) { p.data_dir = c; break; }
        }
    }
    return p;
}

static DiscoveredData discover_data(const fs::path& dir) {
    DiscoveredData d;
    if (dir.empty() || !fs::is_directory(dir)) {
        throw std::runtime_error("找不到 basin_data 目录；可设置 BASIN_DATA_DIR。当前候选目录: " + dir.string());
    }
    for (const auto& ent : fs::directory_iterator(dir)) {
        if (!ent.is_regular_file()) continue;
        const std::string name = ent.path().filename().string();
        if (name.size() < 8 || name.substr(name.size() - 4) != ".csv") continue;
        const std::size_t pre = name.find("_pre");
        if (pre == std::string::npos) continue;
        const std::string id = name.substr(0, pre);
        if (id.empty()) continue;
        const std::string tail = name.substr(pre + 4);  // ".csv" 或 "_severity.csv"
        if (tail == ".csv") {
            d.clean[id] = ent.path();
        } else if (tail.size() > 5 && tail[0] == '_' && tail.substr(tail.size() - 4) == ".csv") {
            const std::string sev = tail.substr(1, tail.size() - 5);
            if (!sev.empty()) d.fault[id][sev] = ent.path();
        }
    }
    // clean 数据为空时由统一入口记录并跳过；不能因此中断后续可用数据集实验。
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
        const auto f = parse_csv_line(line);
        if (f.size() < 9) {
            throw std::runtime_error("basin_experiment_input.csv 第 " + std::to_string(line_no) + " 行少于 9 列");
        }
        ExperimentCase c;
        c.input_target_field = trim(f[0]);
        const std::string target = normalize_excel_cell(f[0]);
        if (all_digits(target)) {
            c.target_is_count = true;
            c.target_count = std::stoi(target);
            if (c.target_count <= 0) throw std::runtime_error("试验品个数必须 >0");
        } else {
            std::istringstream iss(target);
            std::string id;
            while (iss >> id) c.target_ids.push_back(id);
            if (c.target_ids.empty()) throw std::runtime_error("指定试验品集合为空");
        }
        std::string err;
        if (!parse_time_range(f[1], &c.time_range_text, &c.start_date, &c.end_date, &err)) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行: " + err);
        }
        c.sampling_step = std::stoi(normalize_excel_cell(f[2]));
        c.window_length = std::stoi(normalize_excel_cell(f[3]));
        c.tau = std::stoi(normalize_excel_cell(f[4]));
        c.memtable_mb = std::stoi(normalize_excel_cell(f[5]));
        if (c.sampling_step <= 0 || c.window_length <= 0 || c.tau <= 0 || c.memtable_mb <= 0) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行存在非正整数参数");
        }
        if (c.window_length < 2 * c.tau) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行 T < 2*tau，CDiT 无有效样本");
        }
        if (!parse_yes_no(f[6], &c.test_fault) || !parse_yes_no(f[7], &c.print_process_log) ||
            !parse_yes_no(f[8], &c.use_covmeta)) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行的是/否字段无法识别");
        }
        out.push_back(std::move(c));
    }
    // 只有表头而没有实验行表示显式禁用 Basin 实验。
    return out;
}

static std::vector<std::string> choose_dataset_ids(const DiscoveredData& data, const ExperimentCase& c) {
    std::vector<std::string> all;
    all.reserve(data.clean.size());
    for (const auto& kv : data.clean) all.push_back(kv.first);

    if (!c.target_is_count) {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        for (const std::string& id : c.target_ids) {
            if (!data.clean.count(id)) throw std::runtime_error("指定河川不存在 clean 文件: " + id);
            if (seen.insert(id).second) out.push_back(id);
        }
        return out;
    }

    const int n = c.target_count;
    if (n > static_cast<int>(all.size())) {
        throw std::runtime_error("要求试验品个数 " + std::to_string(n) + " 超过可用 clean 数据集 " + std::to_string(all.size()));
    }
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(n));
    if (n == 1) {
        out.push_back(all[all.size() / 2]);
    } else {
        for (int i = 0; i < n; ++i) {
            const double x = static_cast<double>(i) * static_cast<double>(all.size() - 1) / static_cast<double>(n - 1);
            const std::size_t idx = static_cast<std::size_t>(std::llround(x));
            out.push_back(all[idx]);
        }
    }
    return out;
}

static int severity_percent(const std::string& s) {
    std::size_t p = s.find('%');
    if (p == std::string::npos) return 1000000;
    std::string x = s.substr(0, p);
    return all_digits(x) ? std::stoi(x) : 1000000;
}

static std::vector<std::string> severities_for(const DiscoveredData& d,
                                               const std::vector<std::string>& ids,
                                               bool test_fault) {
    std::vector<std::string> out = {""}; // 空串表示无故障 clean。
    if (!test_fault || ids.empty()) return out;
    std::set<std::string> common;
    bool first = true;
    for (const std::string& id : ids) {
        auto fit = d.fault.find(id);
        std::set<std::string> here;
        if (fit != d.fault.end()) for (const auto& kv : fit->second) here.insert(kv.first);
        if (first) { common = std::move(here); first = false; }
        else {
            std::set<std::string> next;
            std::set_intersection(common.begin(), common.end(), here.begin(), here.end(),
                                  std::inserter(next, next.begin()));
            common.swap(next);
        }
    }
    std::vector<std::string> v(common.begin(), common.end());
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
        const int pa = severity_percent(a), pb = severity_percent(b);
        if (pa != pb) return pa < pb;
        return a < b;
    });
    out.insert(out.end(), v.begin(), v.end());
    return out;
}

static DatasetBaseline load_baseline(const std::string& id,
                                     const fs::path& path,
                                     const ExperimentCase& c) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开 clean 数据集: " + path.string());
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("空数据集: " + path.string());
    auto h = parse_csv_line(remove_utf8_bom(line));
    int time_col = -1;
    for (std::size_t i = 0; i < h.size(); ++i) if (trim(h[i]) == "time") time_col = static_cast<int>(i);
    if (time_col < 0) throw std::runtime_error("数据集缺少 time 列: " + path.string());

    DatasetBaseline b;
    b.id = id;
    b.clean_path = path;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto f = parse_csv_line(line);
        if (time_col >= static_cast<int>(f.size())) continue;
        const std::string ts = trim(f[static_cast<std::size_t>(time_col)]);
        if (!date_in_range(ts, c.start_date, c.end_date)) continue;
        if (!b.allowed_timestamp_keys.insert(ts).second) {
            throw std::runtime_error("clean 数据集出现重复时间戳: " + id + " / " + ts);
        }
        b.timestamps.push_back({ts, ts});
    }
    if (b.timestamps.empty()) throw std::runtime_error("时间范围内没有数据: " + id);
    return b;
}

static fs::path choose_data_file(const DiscoveredData& d, const std::string& id, const std::string& severity) {
    if (severity.empty()) return d.clean.at(id);
    auto fit = d.fault.find(id);
    if (fit == d.fault.end() || !fit->second.count(severity)) {
        throw std::runtime_error("缺少故障数据文件: " + id + " / " + severity);
    }
    return fit->second.at(severity);
}

static std::map<std::string, int> header_index(const std::vector<std::string>& h) {
    std::map<std::string, int> out;
    for (std::size_t i = 0; i < h.size(); ++i) out[trim(h[i])] = static_cast<int>(i);
    return out;
}

static void insert_dataset(rocksdb::DB* db,
                           const std::string& id,
                           const fs::path& path,
                           const DatasetBaseline& baseline) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开插入文件: " + path.string());
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("插入文件为空: " + path.string());
    auto idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    for (const char* col : {"time", "tavg", "prec", "Qobs"}) {
        if (!idx.count(col)) throw std::runtime_error("文件缺少列 " + std::string(col) + ": " + path.string());
    }
    const int max_col = std::max({idx["time"], idx["tavg"], idx["prec"], idx["Qobs"]});
    rocksdb::WriteOptions wopt;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto f = parse_csv_line(line);
        if (max_col >= static_cast<int>(f.size())) continue;
        const std::string ts = trim(f[static_cast<std::size_t>(idx["time"])]);
        if (!baseline.allowed_timestamp_keys.count(ts)) continue;
        const std::string key = id + "," + ts;
        const std::string value = trim(f[static_cast<std::size_t>(idx["tavg"])]) + "," +
                                  trim(f[static_cast<std::size_t>(idx["prec"])]) + "," +
                                  trim(f[static_cast<std::size_t>(idx["Qobs"])]);
        rocksdb::Status s = db->Put(wopt, key, value);
        if (!s.ok()) throw std::runtime_error("RocksDB Put 失败: " + s.ToString());
    }
}

static void destroy_db_state(const fs::path& db_dir, const rocksdb::Options& options) {
    rocksdb::Status s = rocksdb::DestroyDB(db_dir.string(), options);
    if (!s.ok() && !s.IsNotFound()) {
        throw std::runtime_error("DestroyDB 失败: " + s.ToString());
    }
    // DestroyDB 正常会删 WAL/SST/MANIFEST；额外 remove_all 处理异常强退后遗留的非 RocksDB 文件。
    std::error_code ec;
    fs::remove_all(db_dir, ec);
}

static std::string yesno(bool v) { return v ? "是" : "否"; }

static void initialize_summary_csv(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("无法创建实验汇总 CSV: " + path.string());
    out.write("\xEF\xBB\xBF", 3);
    out << "插入的试验品的数量,插入的时间范围,插入时的T,推断时的τ,采样步长,MemTable(MB),是否测试故障数据集,是否打印过程日志,是否使用协方差缓存驱动CDiT,插入耗时(s),全数据集全跨度全列范围查询总耗时(s),库内推断部分耗时(s),故障程度,全部页面数,互补页面数,重叠页面数,插入总耗时减库内推断耗时(s)\n";
}

static void append_summary_row(const fs::path& path,
                               const ExperimentCase& c,
                               int dataset_count,
                               double insert_seconds,
                               double query_seconds,
                               const CausalMetaCounters& counters,
                               const std::string& severity) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("无法追加实验汇总 CSV");
    const double derived = insert_seconds - counters.metadata_seconds;
    std::vector<std::string> fields = {
        std::to_string(dataset_count),
        excel_safe_time_range(c.time_range_text),
        std::to_string(c.window_length),
        std::to_string(c.tau),
        std::to_string(c.sampling_step),
        std::to_string(c.memtable_mb),
        yesno(c.test_fault),
        yesno(c.print_process_log),
        yesno(c.use_covmeta)
    };
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) out << ',';
        out << csv_escape(fields[i]);
    }
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

static std::string target_description(const ExperimentCase& c, const std::vector<std::string>& ids) {
    if (c.target_is_count) return "要插入的试验品个数=" + std::to_string(ids.size());
    std::ostringstream oss;
    oss << "指定试验品集合={";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) oss << ',';
        oss << ids[i];
    }
    oss << '}';
    return oss.str();
}

static double run_queries(const ExperimentPaths& paths,
                          const std::vector<std::string>& ids,
                          ExperimentLogger& logger) {
    double total_query_seconds = 0.0;
    for (const std::string& id : ids) {
        logger.line("本次范围查询输入：查询的试验品: " + id + "；查询时间区间:-∞到+∞；查询列:T,P,Q");
        logger.line("查询结果如下");

        const Clock::time_point one0 = Clock::now();
        std::string err;
        auto segs = causal_meta_query_dataset_from_file(paths.meta_dir.string(), id, "", "", {"T", "P", "Q"}, &err);
        if (!err.empty()) throw std::runtime_error(err);
        std::vector<std::string> result_lines;
        result_lines.reserve(segs.size());
        for (const auto& seg : segs) {
            result_lines.push_back(causal_meta_format_query_segment(seg, {"T", "P", "Q"}));
        }
        const double one_seconds = std::chrono::duration<double>(Clock::now() - one0).count();
        total_query_seconds += one_seconds;

        // 查询计时不把终端/日志文件 I/O 算入查询算法本身，但严格覆盖“读 C 文件 -> 筛选/投影 -> 结果字符串组织”。
        for (const auto& line : result_lines) logger.line(line);
        {
            std::ostringstream oss;
            oss << "本次范围查询小节：耗时:" << std::fixed << std::setprecision(6) << one_seconds << 's';
            logger.line(oss.str());
        }
        logger.line();
    }
    return total_query_seconds;
}

static void run_one(const ExperimentPaths& paths,
                    const DiscoveredData& discovered,
                    const ExperimentCase& c,
                    const std::vector<std::string>& ids,
                    const std::string& severity,
                    int suite_index,
                    int run_index,
                    ExperimentLogger& logger) {
    // 清理时间不计入任何实验耗时。
    fs::remove_all(paths.meta_dir);
    fs::create_directories(paths.meta_dir);

    causal_meta_reset_all();
    causal_meta_set_enabled_cpp(true);  // 本实验默认开启元数据维护。
    causal_meta_set_metadata_dir(paths.meta_dir.string());
    causal_meta_set_page_logging(c.print_process_log);
    causal_meta_set_log_sink(&ExperimentLogger::causal_sink, &logger);

    std::map<std::string, DatasetBaseline> baselines;
    for (const std::string& id : ids) {
        DatasetBaseline b = load_baseline(id, discovered.clean.at(id), c);
        CausalMetaDatasetConfig cfg;
        cfg.dataset_id = id;
        cfg.display_name = id;
        cfg.variable_names = {"T", "P", "Q"};
        cfg.ordered_timestamps = b.timestamps;
        cfg.sampling_step = c.sampling_step;
        cfg.window_length = c.window_length;
        cfg.tau = c.tau;
        cfg.alpha = 0.01;
        cfg.infer_self = false;
        cfg.use_covariance_cache_cdit = c.use_covmeta;
        std::string err;
        if (!causal_meta_register_dataset(cfg, &err)) throw std::runtime_error(err);
        baselines.emplace(id, std::move(b));
    }

    rocksdb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = static_cast<std::size_t>(c.memtable_mb) * 1024ULL * 1024ULL;

    // 一个 active + 一个 immutable；min_merge=1 让每个 immutable 单独形成 Flush L0 文件。
    // 这样 collector 的“一次成功 Flush 表”与论文/实验中的 Immutable MemTable 页面保持一一对应。
    options.max_write_buffer_number = 2;
    options.min_write_buffer_number_to_merge = 1;
    options.max_background_flushes = 1;
    options.max_background_compactions = 1;

    std::string attach_error;
    if (!causal_meta_attach_to_rocksdb_options(&options, &attach_error)) {
        throw std::runtime_error(attach_error);
    }

    destroy_db_state(paths.db_dir, options);

    logger.line("第" + std::to_string(suite_index) + "套-第" + std::to_string(run_index) + "次实验");
    logger.line("数据库参数设置：MemTable=" + std::to_string(c.memtable_mb) + "MB");
    logger.line("本次插入实验参数设置：插入时间范围=" + c.time_range_text +
                "；T=" + std::to_string(c.window_length) +
                "；τ=" + std::to_string(c.tau) +
                "；采样步长=" + std::to_string(c.sampling_step) +
                "；" + target_description(c, ids) +
                "；是否使用协方差缓存驱动CDiT：" + yesno(c.use_covmeta) +
                "；故障程度=" + (severity.empty() ? "无" : severity) + "；");
    logger.line();

    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Status open_status = rocksdb::DB::Open(options, paths.db_dir.string(), &db);
    if (!open_status.ok()) throw std::runtime_error("RocksDB Open 失败: " + open_status.ToString());

    const Clock::time_point insert0 = Clock::now();
    try {
        for (const std::string& id : ids) {
            insert_dataset(db.get(), id, choose_data_file(discovered, id, severity), baselines.at(id));
        }

        // 强制最后一个尚未达到阈值的 mutable MemTable 也成为本次实验页面；wait=true 等到 Flush 完成。
        rocksdb::FlushOptions fopt;
        fopt.wait = true;
        fopt.listener_wait = true;
        rocksdb::Status fs = db->Flush(fopt);
        if (!fs.ok()) throw std::runtime_error("RocksDB Flush 失败: " + fs.ToString());

        causal_meta_wait_for_idle();
        std::string persist_err;
        if (!causal_meta_persist_all(&persist_err)) throw std::runtime_error(persist_err);
    } catch (...) {
        // 即使实验中途异常，也尽力完成通用关闭前动作，再让异常继续上抛。
        std::string ignored;
        causal_meta_before_db_close(&ignored);
        db.reset();
        throw;
    }
    const double insert_seconds = std::chrono::duration<double>(Clock::now() - insert0).count();
    CausalMetaCounters counters = causal_meta_get_counters();

    logger.line("本次插入完毕 | 耗时：" + [&] {
        std::ostringstream oss; oss << std::fixed << std::setprecision(6) << insert_seconds << 's'; return oss.str();
    }() + "；");
    logger.line();

    const double query_seconds = run_queries(paths, ids, logger);
    logger.line("全数据集全跨度全列范围查询总耗时:" + [&] {
        std::ostringstream oss; oss << std::fixed << std::setprecision(6) << query_seconds << 's'; return oss.str();
    }());
    logger.line();

    append_summary_row(paths.summary_csv, c, static_cast<int>(ids.size()),
                       insert_seconds, query_seconds, counters, severity);

    // 关闭前显式执行通用 barrier + 持久化。前面已经持久化且没有新脏页时，此调用为快速 no-op。
    std::string close_err;
    if (!causal_meta_before_db_close(&close_err)) throw std::runtime_error(close_err);
    db.reset();
    causal_meta_clear_log_sink();

    // 每次实验之后恢复 RocksDB 为空库。此时间严格位于计时之外。
    // 元数据目录不在这里删：下一次实验开头会覆盖清空，因此最后一次实验结束后仍能检查 C/B 文件。
    destroy_db_state(paths.db_dir, options);
}

} // 匿名命名空间结束

int run_basin_experiments() {
    try {
        const ExperimentPaths paths = resolve_paths();
        ExperimentLogger logger(paths.print_log);
        logger.initialize();
        initialize_summary_csv(paths.summary_csv);

        if (paths.data_dir.empty() || !fs::is_directory(paths.data_dir)) {
            logger.line("未提供 Basin 数据目录，按数据集公开策略跳过河川径流实验。");
            return 0;
        }
        const DiscoveredData discovered = discover_data(paths.data_dir);
        const std::vector<ExperimentCase> cases = load_cases(paths.input_csv);
        if (discovered.clean.empty() || cases.empty()) {
            logger.line("Basin 数据或实验配置为空，跳过河川径流实验。");
            return 0;
        }

        int suite_index = 0;
        for (const ExperimentCase& c : cases) {
            ++suite_index;
            const std::vector<std::string> ids = choose_dataset_ids(discovered, c);
            const std::vector<std::string> severities = severities_for(discovered, ids, c.test_fault);
            if (c.test_fault && severities.size() == 1) {
                logger.line("警告：第" + std::to_string(suite_index) + "套实验要求故障数据，但所选数据集没有共同故障程度；只执行无故障版本。");
            }
            int run_index = 0;
            for (const std::string& severity : severities) {
                ++run_index;
                run_one(paths, discovered, c, ids, severity, suite_index, run_index, logger);
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "河川径流实验失败: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "河川径流实验失败: 未知异常" << std::endl;
        return 2;
    }
}
