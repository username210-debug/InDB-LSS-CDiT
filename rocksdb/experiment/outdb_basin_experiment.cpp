/*
 * 本文件实现 Basin 数据集的库外处理对照实验，负责数据写入、重新读取、窗口分析和分项计时。
 */

#include "experiment/outdb_basin_experiment.h"

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

struct ExperimentCase {
    bool target_is_count = false;
    int target_count = 0;
    std::vector<std::string> target_ids;

    std::string time_range_text;
    std::optional<int> start_date;
    std::optional<int> end_date;

    int sampling_step = 1;
    int window_length = 365;
    int tau = 5;
    int memtable_mb = 2;
    bool use_covmeta = false;
};

struct DatasetInfo {
    std::string id;
    fs::path path;
    std::vector<std::string> timestamps;
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
            // 实验运行过程中即可打开文件查看已经完成的打印内容。
            stream_.flush();
        }
    }

private:
    fs::path print_log_;
    std::ofstream stream_;
};

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

// RFC4180 风格的一行 CSV 解析；同时支持 Excel 单元格中的双引号转义。
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
    // parse_csv_line 已经去掉 CSV 字段外层引号；这里继续处理 Excel 的 ="..."。
    if (s.size() >= 3 && s[0] == '=' && s[1] == '"' && s.back() == '"') {
        s = s.substr(2, s.size() - 3);
    }
    return trim(s);
}

static std::string excel_safe_time_range(const std::string& s) {
    // 以 '-' 开头的 -∞到+∞ 直接放进 CSV 时，Excel 可能把它识别成公式。
    if (s == "-∞到+∞") return "=\"-∞到+∞\"";
    return s;
}

static bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!std::isdigit(c)) return false;
    }
    return true;
}

static bool parse_yes_no(const std::string& raw, bool* out) {
    const std::string s = normalize_excel_cell(raw);
    std::string low;
    low.reserve(s.size());
    for (unsigned char c : s) low.push_back(static_cast<char>(std::tolower(c)));
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

static std::string yesno(bool v) {
    return v ? "是" : "否";
}

static int parse_basin_date(const std::string& raw) {
    const std::string s = trim(raw);
    int y = 0;
    int m = 0;
    int d = 0;
    char sep1 = 0;
    char sep2 = 0;
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
    const std::string s = normalize_excel_cell(raw);
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
    p.input_csv = p.root / "experiment" / "outdb_basin_experiment_input.csv";
    p.db_dir = p.root / "rdb_basin_outdb_experiment";
    p.log_dir = p.root / "experiment_log";
    p.summary_csv = p.log_dir / "对河川径流的入库+库外推断实验统计.csv";
    p.print_log = p.log_dir / "河川径流入库+库外推断实验全部打印日志.txt";

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
            if (fs::is_directory(c, ec)) {
                p.data_dir = c;
                break;
            }
        }
    }
    return p;
}

static std::map<std::string, fs::path> discover_clean_data(const fs::path& dir) {
    if (dir.empty() || !fs::is_directory(dir)) {
        throw std::runtime_error("找不到 basin_data 目录；可设置 BASIN_DATA_DIR。当前目录: " + dir.string());
    }

    std::map<std::string, fs::path> out;
    for (const auto& ent : fs::directory_iterator(dir)) {
        if (!ent.is_regular_file()) continue;
        const std::string name = ent.path().filename().string();
        if (name.size() <= 8 || name.substr(name.size() - 8) != "_pre.csv") continue;
        const std::string id = name.substr(0, name.size() - 8);
        if (!id.empty()) out[id] = ent.path();
    }
    // 返回空映射，由实验入口统一记录并跳过该数据集。
    return out;
}

static std::vector<ExperimentCase> load_cases(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开实验输入文件: " + path.string());

    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("实验输入文件为空: " + path.string());
    line = remove_utf8_bom(line);

    std::vector<ExperimentCase> cases;
    int line_no = 1;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;

        const std::vector<std::string> f = parse_csv_line(line);
        if (f.size() < 7) {
            throw std::runtime_error("outdb_basin_experiment_input.csv 第 " +
                                     std::to_string(line_no) + " 行少于 7 列");
        }

        ExperimentCase c;
        const std::string target = normalize_excel_cell(f[0]);
        if (all_digits(target)) {
            c.target_is_count = true;
            c.target_count = std::stoi(target);
            if (c.target_count <= 0) {
                throw std::runtime_error("第 " + std::to_string(line_no) + " 行试验品个数必须 > 0");
            }
        } else {
            std::istringstream iss(target);
            std::string id;
            while (iss >> id) c.target_ids.push_back(id);
            if (c.target_ids.empty()) {
                throw std::runtime_error("第 " + std::to_string(line_no) + " 行指定试验品集合为空");
            }
        }

        std::string err;
        if (!parse_time_range(f[1], &c.time_range_text, &c.start_date, &c.end_date, &err)) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行: " + err);
        }

        try {
            c.sampling_step = std::stoi(normalize_excel_cell(f[2]));
            c.window_length = std::stoi(normalize_excel_cell(f[3]));
            c.tau = std::stoi(normalize_excel_cell(f[4]));
            c.memtable_mb = std::stoi(normalize_excel_cell(f[5]));
        } catch (const std::exception&) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行存在无法解析的整数参数");
        }
        if (c.sampling_step <= 0 || c.window_length <= 0 || c.tau <= 0 || c.memtable_mb <= 0) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行存在非正整数参数");
        }
        if (c.window_length < 2 * c.tau) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行 T < 2*tau，CDiT 无有效样本");
        }
        if (!parse_yes_no(f[6], &c.use_covmeta)) {
            throw std::runtime_error("第 " + std::to_string(line_no) +
                                     " 行“是否使用协方差缓存驱动CDiT”无法识别");
        }

        cases.push_back(std::move(c));
    }

    // 只有表头而没有实验行表示显式禁用 Basin 入库+库外推断实验。
    return cases;
}

static std::vector<std::string> choose_dataset_ids(
        const std::map<std::string, fs::path>& discovered,
        const ExperimentCase& c) {
    std::vector<std::string> all;
    all.reserve(discovered.size());
    for (const auto& kv : discovered) all.push_back(kv.first);

    if (!c.target_is_count) {
        std::vector<std::string> out;
        std::set<std::string> seen;
        for (const std::string& id : c.target_ids) {
            if (!discovered.count(id)) {
                throw std::runtime_error("指定河川不存在 *_pre.csv: " + id);
            }
            if (seen.insert(id).second) out.push_back(id);
        }
        return out;
    }

    if (c.target_count > static_cast<int>(all.size())) {
        throw std::runtime_error("要求试验品个数 " + std::to_string(c.target_count) +
                                 " 超过可用数据集个数 " + std::to_string(all.size()));
    }

    // 与已经完成的 basin_experiment 保持一致：按 ID 排序后均匀抽取指定数量。
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(c.target_count));
    if (c.target_count == 1) {
        out.push_back(all[all.size() / 2]);
    } else {
        for (int i = 0; i < c.target_count; ++i) {
            const double x = static_cast<double>(i) * static_cast<double>(all.size() - 1) /
                             static_cast<double>(c.target_count - 1);
            const std::size_t idx = static_cast<std::size_t>(std::llround(x));
            out.push_back(all[idx]);
        }
    }
    return out;
}

static std::map<std::string, int> header_index(const std::vector<std::string>& header) {
    std::map<std::string, int> out;
    for (std::size_t i = 0; i < header.size(); ++i) {
        out[trim(header[i])] = static_cast<int>(i);
    }
    return out;
}

static DatasetInfo load_dataset_info(const std::string& id,
                                     const fs::path& path,
                                     const ExperimentCase& c) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开数据集: " + path.string());

    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("空数据集: " + path.string());
    const auto idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    auto t_it = idx.find("time");
    if (t_it == idx.end()) throw std::runtime_error("数据集缺少 time 列: " + path.string());
    const int time_col = t_it->second;

    DatasetInfo info;
    info.id = id;
    info.path = path;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto f = parse_csv_line(line);
        if (time_col >= static_cast<int>(f.size())) continue;
        const std::string ts = trim(f[static_cast<std::size_t>(time_col)]);
        if (!date_in_range(ts, c.start_date, c.end_date)) continue;
        if (!info.allowed_timestamp_keys.insert(ts).second) {
            throw std::runtime_error("数据集出现重复时间戳: " + id + " / " + ts);
        }
        info.timestamps.push_back(ts);
    }

    if (info.timestamps.empty()) {
        throw std::runtime_error("插入时间范围内没有数据: " + id);
    }
    return info;
}

static void insert_dataset(rocksdb::DB* db, const DatasetInfo& info) {
    std::ifstream in(info.path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开插入文件: " + info.path.string());

    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("插入文件为空: " + info.path.string());
    const auto idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    for (const char* col : {"time", "tavg", "prec", "Qobs"}) {
        if (!idx.count(col)) {
            throw std::runtime_error("文件缺少列 " + std::string(col) + ": " + info.path.string());
        }
    }
    const int max_col = std::max({idx.at("time"), idx.at("tavg"), idx.at("prec"), idx.at("Qobs")});

    rocksdb::WriteOptions wopt;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto f = parse_csv_line(line);
        if (max_col >= static_cast<int>(f.size())) continue;

        const std::string ts = trim(f[static_cast<std::size_t>(idx.at("time"))]);
        if (!info.allowed_timestamp_keys.count(ts)) continue;

        // 保持用户既定的最小侵入键值格式：key=数据集名,时间戳；value=T,P,Q。
        const std::string key = info.id + "," + ts;
        const std::string value = trim(f[static_cast<std::size_t>(idx.at("tavg"))]) + "," +
                                  trim(f[static_cast<std::size_t>(idx.at("prec"))]) + "," +
                                  trim(f[static_cast<std::size_t>(idx.at("Qobs"))]);
        const rocksdb::Status s = db->Put(wopt, key, value);
        if (!s.ok()) throw std::runtime_error("RocksDB Put 失败: " + s.ToString());
    }
}

static std::vector<double> parse_db_value(const std::string& value,
                                          const std::string& dataset_id,
                                          const std::string& ts) {
    std::vector<double> out;
    out.reserve(3);
    std::size_t begin = 0;
    for (;;) {
        const std::size_t p = value.find(',', begin);
        const std::string field = trim(value.substr(begin, p == std::string::npos ? p : p - begin));
        try {
            std::size_t used = 0;
            const double x = std::stod(field, &used);
            if (used != field.size() || !std::isfinite(x)) {
                throw std::runtime_error("非有限数");
            }
            out.push_back(x);
        } catch (const std::exception&) {
            throw std::runtime_error("RocksDB value 数值解析失败: " + dataset_id + " / " + ts +
                                     " / value=" + value);
        }
        if (p == std::string::npos) break;
        begin = p + 1;
    }
    if (out.size() != 3) {
        throw std::runtime_error("RocksDB value 列数不是 3: " + dataset_id + " / " + ts +
                                 " / value=" + value);
    }
    return out;
}

static void destroy_db_state(const fs::path& db_dir, const rocksdb::Options& options) {
    // RocksDB 11 的 DestroyDB 会先尝试创建/锁定 db_dir/LOCK；目录根本不存在时可返回 IOError。
    // 空目录不存在本身已经等价于“空库”，所以先显式判断，避免第 1 次实验在全新环境中被误判为清库失败。
    std::error_code exists_ec;
    const bool exists = fs::exists(db_dir, exists_ec);
    if (exists_ec) {
        throw std::runtime_error("检查数据库目录失败: " + db_dir.string() + " / " + exists_ec.message());
    }
    if (!exists) return;

    const rocksdb::Status s = rocksdb::DestroyDB(db_dir.string(), options);
    if (!s.ok() && !s.IsNotFound()) {
        throw std::runtime_error("DestroyDB 失败: " + s.ToString());
    }

    // DestroyDB 会清理 RocksDB 自身的 SST/WAL/MANIFEST；这里额外处理异常强退遗留的非 RocksDB 文件。
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
        // 异常传播路径只做 best-effort 清理，不覆盖最初的实验异常。
    }
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
        // 本次实验只在“时间上相邻且完整因果图（含滞后集合）完全一致”时合并打印区间。
        segments->back().end_label = end_label;
        return;
    }
    segments->push_back({start_label, end_label, std::move(graph)});
}

static std::string format_graph_tpq(const CDiTResult& graph) {
    // CDiTResult 的 key 是 (1-based source,target) 的 std::map，变量 1/2/3 分别为 T/P/Q，
    // 因而下面显式整理后稳定满足 T、P、Q 的原因优先、结果次优先排序要求。
    static const std::vector<std::string> names = {"T", "P", "Q"};
    struct Item {
        int source = 0;
        int target = 0;
        std::vector<int> lags;
    };
    std::vector<Item> items;
    for (const auto& kv : graph.lags_by_edge) {
        if (kv.second.empty()) continue;
        Item item;
        item.source = kv.first.first;
        item.target = kv.first.second;
        item.lags = kv.second;
        std::sort(item.lags.begin(), item.lags.end());
        item.lags.erase(std::unique(item.lags.begin(), item.lags.end()), item.lags.end());
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.source != b.source) return a.source < b.source;
        return a.target < b.target;
    });

    auto name_of = [&](int x) -> std::string {
        if (x >= 1 && x <= static_cast<int>(names.size())) {
            return names[static_cast<std::size_t>(x - 1)];
        }
        return "X" + std::to_string(x);
    };

    std::ostringstream oss;
    for (const Item& item : items) {
        oss << name_of(item.source) << "→" << name_of(item.target) << "滞后=";
        for (int lag : item.lags) oss << lag << ',';
        oss << "; ";
    }
    return oss.str();
}

static std::vector<std::string> sampled_timestamps(const DatasetInfo& info, int sampling_step) {
    std::vector<std::string> out;
    out.reserve((info.timestamps.size() + static_cast<std::size_t>(sampling_step) - 1) /
                static_cast<std::size_t>(sampling_step));
    for (std::size_t raw_pos = 0; raw_pos < info.timestamps.size(); ++raw_pos) {
        if (raw_pos % static_cast<std::size_t>(sampling_step) == 0) {
            out.push_back(info.timestamps[raw_pos]);
        }
    }
    return out;
}

static std::vector<InferenceSegment> infer_one_dataset_from_rocksdb(
        rocksdb::DB* db,
        const DatasetInfo& info,
        const ExperimentCase& c,
        QueryTiming* timing) {
    const std::vector<std::string> sampled = sampled_timestamps(info, c.sampling_step);
    const std::size_t T = static_cast<std::size_t>(c.window_length);
    const std::size_t complete_window_count = sampled.size() / T;

    std::vector<InferenceSegment> segments;
    segments.reserve(complete_window_count);
    const rocksdb::ReadOptions ropt;

    for (std::size_t w = 0; w < complete_window_count; ++w) {
        const std::size_t begin = w * T;
        const std::size_t end = begin + T;
        std::vector<std::vector<double>> data;
        data.reserve(T);

        // 读取计时只覆盖 RocksDB Get + value 解析；每个完整窗口读取后立刻进入推断。
        const Clock::time_point read0 = Clock::now();
        for (std::size_t i = begin; i < end; ++i) {
            const std::string& ts = sampled[i];
            const std::string key = info.id + "," + ts;
            std::string value;
            const rocksdb::Status s = db->Get(ropt, key, &value);
            if (!s.ok()) {
                throw std::runtime_error("RocksDB DB::Get 读取失败: key=" + key + " / " + s.ToString());
            }
            data.push_back(parse_db_value(value, info.id, ts));
        }
        timing->read_seconds += seconds_between(read0, Clock::now());

        // 推断计时只覆盖 CDiT/LSS-CDiT 本体；日志组织和相邻图区间合并均不混入该计时。
        const Clock::time_point infer0 = Clock::now();
        CDiTResult graph = c.use_covmeta
                ? run_cdit_covmeta(data, c.tau, 0.01, false, nullptr, nullptr)
                : run_cdit_original(data, c.tau, 0.01, false, nullptr, nullptr);
        timing->inference_seconds += seconds_between(infer0, Clock::now());

        append_or_merge_segment(&segments, sampled[begin], sampled[end - 1], std::move(graph));
    }
    return segments;
}

static std::string summary_target_field(const ExperimentCase& c,
                                        const std::vector<std::string>& selected_ids) {
    if (c.target_is_count) return std::to_string(selected_ids.size());
    std::ostringstream oss;
    for (std::size_t i = 0; i < selected_ids.size(); ++i) {
        if (i) oss << ' ';
        oss << selected_ids[i];
    }
    return oss.str();
}

static void initialize_summary_csv(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("无法创建实验统计 CSV: " + path.string());
    // UTF-8 BOM，避免 Windows Excel 直接双击时按本地 ANSI/GBK 误判编码。
    out.write("\xEF\xBB\xBF", 3);
    out << "试验品个数或指定试验品集合,插入时间起止范围,采样步长,完整子随机过程长度T,推断时的τ,MemTable容量(MB),是否使用协方差缓存驱动CDiT,总耗时(s),插入总耗时(s),库外推断中仅读取部分耗时(s),库外推断中仅推断部分耗时(s)\n";
    out.flush();
}

static void append_summary_row(const fs::path& path,
                               const ExperimentCase& c,
                               const std::vector<std::string>& selected_ids,
                               double total_seconds,
                               double insert_seconds,
                               double read_seconds,
                               double inference_seconds) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("无法追加实验统计 CSV: " + path.string());

    const std::vector<std::string> fields = {
        summary_target_field(c, selected_ids),
        excel_safe_time_range(c.time_range_text),
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
    // 一次实验结束立刻落盘一行，长实验过程中即可打开 CSV 检查已有结果。
    out.flush();
}

static QueryTiming run_outdb_inference(rocksdb::DB* db,
                                       const std::map<std::string, DatasetInfo>& infos,
                                       const std::vector<std::string>& ids,
                                       const ExperimentCase& c,
                                       ExperimentLogger& logger) {
    QueryTiming timing;

    logger.line("本次范围推断输入：实验品个数=" + std::to_string(ids.size()) +
                "；推断时间区间=-∞到+∞；参与列=T,P,Q；采样步长=" +
                std::to_string(c.sampling_step) + "；T=" + std::to_string(c.window_length) +
                "；τ=" + std::to_string(c.tau) + "；");

    for (const std::string& id : ids) {
        logger.line("试验品：" + id);
        // 小节总耗时只覆盖库外处理本身，不把控制台/日志文件 I/O 混入实验算法时间。
        const Clock::time_point one0 = Clock::now();
        const std::vector<InferenceSegment> segments =
                infer_one_dataset_from_rocksdb(db, infos.at(id), c, &timing);
        timing.total_seconds += seconds_between(one0, Clock::now());
        for (const InferenceSegment& seg : segments) {
            logger.line(seg.start_label + "至" + seg.end_label + " | 推断：" +
                        format_graph_tpq(seg.graph));
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
                               const std::vector<std::string>& ids,
                               int experiment_index,
                               ExperimentLogger& logger) {
    rocksdb::Options options = make_options(c);

    // 清理发生在本次实验总计时之前；第 1 次和每次实验之间都保证为空库。
    destroy_db_state(paths.db_dir, options);

    // “总耗时”从本次实验正式开始计到库外推断结束；只把前后清库严格排除。
    const Clock::time_point total0 = Clock::now();

    // 本实验明确关闭库内元数据维护。attach 调用在开关关闭时会直接返回，不注册 listener/collector。
    causal_meta_reset_all();
    causal_meta_set_enabled_cpp(false);
    std::string attach_error;
    if (!causal_meta_attach_to_rocksdb_options(&options, &attach_error)) {
        throw std::runtime_error(attach_error);
    }

    std::map<std::string, DatasetInfo> infos;
    for (const std::string& id : ids) {
        infos.emplace(id, load_dataset_info(id, discovered.at(id), c));
    }

    logger.line("河川径流第" + std::to_string(experiment_index) + "次实验");
    logger.line("数据库参数设置：MemTable=" + std::to_string(c.memtable_mb) + "MB");
    logger.line("本此插入实验参数设置：插入时间范围=" + c.time_range_text +
                "；要插入的试验品个数=" + std::to_string(ids.size()) +
                "；是否使用协方差缓存驱动CDiT：" + yesno(c.use_covmeta) + "；");
    logger.line();

    std::unique_ptr<rocksdb::DB> db;
    try {
        rocksdb::Status s = rocksdb::DB::Open(options, paths.db_dir.string(), &db);
        if (!s.ok()) throw std::runtime_error("RocksDB Open 失败: " + s.ToString());

        const Clock::time_point insert0 = Clock::now();
        for (const std::string& id : ids) insert_dataset(db.get(), infos.at(id));

        // 显式把最后一个未达到 MemTable 阈值的 mutable MemTable 也同步刷成 SST。
        rocksdb::FlushOptions fopt;
        fopt.wait = true;
        s = db->Flush(fopt);
        if (!s.ok()) throw std::runtime_error("RocksDB Flush 失败: " + s.ToString());

        // 关闭插入实例；后续必须重新 Open，再从 RocksDB 公共读取接口取数。
        db.reset();
        const double insert_seconds = seconds_between(insert0, Clock::now());

        rocksdb::Options read_open_options = options;
        read_open_options.create_if_missing = false;
        s = rocksdb::DB::Open(read_open_options, paths.db_dir.string(), &db);
        if (!s.ok()) throw std::runtime_error("RocksDB 重新打开失败: " + s.ToString());

        const QueryTiming query_timing = run_outdb_inference(db.get(), infos, ids, c, logger);
        db.reset();

        const double total_seconds = seconds_between(total0, Clock::now());
        append_summary_row(paths.summary_csv, c, ids, total_seconds, insert_seconds,
                           query_timing.read_seconds, query_timing.inference_seconds);
    } catch (...) {
        db.reset();
        destroy_db_state_noexcept(paths.db_dir, options);
        throw;
    }

    // 每次实验完成后再次恢复为空库；这个清理时间不进入任何统计计时。
    destroy_db_state(paths.db_dir, options);
}

}  // 匿名命名空间结束

int run_outdb_basin_experiments() {
    try {
        const ExperimentPaths paths = resolve_paths();
        ExperimentLogger logger(paths.print_log);
        logger.initialize();
        initialize_summary_csv(paths.summary_csv);

        if (paths.data_dir.empty() || !fs::is_directory(paths.data_dir)) {
            logger.line("未提供 Basin 数据目录，按数据集公开策略跳过河川径流入库+库外推断实验。");
            return 0;
        }
        const std::map<std::string, fs::path> discovered = discover_clean_data(paths.data_dir);
        const std::vector<ExperimentCase> cases = load_cases(paths.input_csv);
        if (discovered.empty() || cases.empty()) {
            logger.line("Basin 数据或实验配置为空，跳过河川径流入库+库外推断实验。");
            return 0;
        }

        // 即使上次实验强退，也在第 1 次实验启动前先清空既有 RocksDB 状态；不计时。
        rocksdb::Options cleanup_options;
        cleanup_options.create_if_missing = true;
        destroy_db_state(paths.db_dir, cleanup_options);

        int experiment_index = 0;
        for (const ExperimentCase& c : cases) {
            const std::vector<std::string> ids = choose_dataset_ids(discovered, c);
            ++experiment_index;
            run_one_experiment(paths, discovered, c, ids, experiment_index, logger);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "河川径流入库+库外推断实验失败: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "河川径流入库+库外推断实验失败: 未知异常" << std::endl;
        return 2;
    }
}
