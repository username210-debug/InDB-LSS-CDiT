/*
 * 本文件实现 Fluxnet 数据集的库外处理对照实验，负责数据写入、重新读取、窗口分析和分项计时。
 */

#include "experiment/outdb_fluxnet_experiment.h"

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
#include <limits>
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

enum class Resolution {
    kHH,
    kDD,
    kWW,
    kMM,
    kYY,
};

struct CivilTime {
    int year = 0;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
};

struct ExperimentCase {
    bool target_is_count = false;
    int target_count = 0;
    std::vector<std::string> target_ids;

    std::string time_range_text;
    std::optional<long long> start_time;
    std::optional<long long> end_time;

    Resolution resolution = Resolution::kDD;
    std::vector<std::string> columns;
    int window_length = 365;
    int tau = 5;
    int memtable_mb = 2;
    bool use_covmeta = false;

    // 用户给出的文字说明中曾出现“合并时的θ”列，但当前已经提供的
    // RocksDB 基线代码没有 merge_cg.*，同类 outdb_basin_experiment 采用
    // “相邻完整因果图完全相同才合并”的确定性规则。因此这里只兼容读取该列，
    // 不擅自发明新的 θ 图距离语义；真正的打印合并规则见 append_or_merge_segment()。
    std::optional<int> compatibility_merge_theta;
};

struct ResolutionFile {
    std::string dataset_id;
    fs::path path;
};

struct SiteData {
    std::string site_id;
    std::string folder_name;
    std::map<Resolution, ResolutionFile> by_resolution;
};

struct DiscoveredData {
    std::map<std::string, SiteData> sites;
    std::vector<std::string> sorted_site_ids;
};

struct TimestampInfo {
    std::string key;
    std::string label;
};

struct DatasetInfo {
    std::string site_id;
    std::string dataset_id;
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
            // 每打印一行立即刷新，长实验运行过程中即可打开日志查看进度。
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

static void replace_all(std::string* s, const std::string& from, const std::string& to) {
    if (!s || from.empty()) return;
    std::size_t p = 0;
    while ((p = s->find(from, p)) != std::string::npos) {
        s->replace(p, from.size(), to);
        p += to.size();
    }
}

// RFC4180 风格单行 CSV 解析；同时支持 Excel 单元格中的双引号转义。
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
    // 同时容忍 Excel/人工编辑后残留的一层普通双引号。
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return trim(s);
}

static std::string excel_safe_time_range(const std::string& s) {
    // 以 '-' 开头的 -∞到+∞ 直接写入 CSV 时，Excel 可能将其识别为公式。
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

static bool leap_year(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m) {
    static const int kDays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && leap_year(y)) return 29;
    return kDays[m];
}

static bool valid_dataset_time(const CivilTime& t) {
    if (t.year < 1 || t.month < 1 || t.month > 12) return false;
    if (t.day < 1 || t.day > days_in_month(t.year, t.month)) return false;
    if (t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59) return false;
    return true;
}

static bool valid_query_boundary(const CivilTime& t) {
    // 用户给出的示例包含 2011-06-31。查询边界只用于与真实数据时间戳做有序比较，
    // 因此这里允许“1..31”的日字段，避免把这类月底上界写法直接判死；
    // 数据文件自身的 TIMESTAMP 仍使用 valid_dataset_time() 做严格日历校验。
    if (t.year < 1 || t.month < 1 || t.month > 12) return false;
    if (t.day < 1 || t.day > 31) return false;
    if (t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59) return false;
    return true;
}

static long long scalar_time(const CivilTime& t) {
    return static_cast<long long>(t.year) * 100000000LL +
           static_cast<long long>(t.month) * 1000000LL +
           static_cast<long long>(t.day) * 10000LL +
           static_cast<long long>(t.hour) * 100LL +
           static_cast<long long>(t.minute);
}

static std::string display_time(const CivilTime& t) {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << t.year
        << '-' << std::setw(2) << t.month
        << '-' << std::setw(2) << t.day
        << std::setfill(' ') << ' ' << t.hour << ':' << t.minute;
    return oss.str();
}

static std::vector<std::string> split_char(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

static bool parse_human_time(const std::string& raw, CivilTime* out) {
    std::string s = trim(raw);
    std::replace(s.begin(), s.end(), '/', '-');
    const std::size_t p = s.find_first_of(" Tt");
    const std::string date = trim(p == std::string::npos ? s : s.substr(0, p));
    const std::string time = trim(p == std::string::npos ? std::string() : s.substr(p + 1));
    const auto d = split_char(date, '-');
    if (d.size() != 3 || !all_digits(trim(d[0])) ||
        !all_digits(trim(d[1])) || !all_digits(trim(d[2]))) {
        return false;
    }

    CivilTime t;
    try {
        t.year = std::stoi(trim(d[0]));
        t.month = std::stoi(trim(d[1]));
        t.day = std::stoi(trim(d[2]));
        if (!time.empty()) {
            const auto hm = split_char(time, ':');
            if (hm.empty() || hm.size() > 2 || !all_digits(trim(hm[0])) ||
                (hm.size() == 2 && !all_digits(trim(hm[1])))) {
                return false;
            }
            t.hour = std::stoi(trim(hm[0]));
            t.minute = hm.size() == 2 ? std::stoi(trim(hm[1])) : 0;
        }
    } catch (...) {
        return false;
    }

    if (!valid_query_boundary(t)) return false;
    *out = t;
    return true;
}

static bool parse_time_range(const std::string& raw,
                             std::string* normalized,
                             std::optional<long long>* start,
                             std::optional<long long>* end,
                             std::string* error) {
    const std::string s = normalize_excel_cell(raw);
    std::string low;
    low.reserve(s.size());
    for (unsigned char c : s) low.push_back(static_cast<char>(std::tolower(c)));
    if (s == "-∞到+∞" || low == "-inf到+inf") {
        *normalized = "-∞到+∞";
        start->reset();
        end->reset();
        return true;
    }

    const std::string marker = "到";
    const std::size_t p = s.find(marker);
    if (p == std::string::npos) {
        if (error) *error = "无法解析查询时间起止范围: " + raw;
        return false;
    }

    CivilTime a;
    CivilTime b;
    if (!parse_human_time(s.substr(0, p), &a) ||
        !parse_human_time(s.substr(p + marker.size()), &b)) {
        if (error) *error = "查询时间起止范围格式非法: " + raw;
        return false;
    }
    const long long sa = scalar_time(a);
    const long long sb = scalar_time(b);
    if (sa > sb) {
        if (error) *error = "查询时间起点晚于终点: " + raw;
        return false;
    }

    *normalized = display_time(a) + "到" + display_time(b);
    *start = sa;
    *end = sb;
    return true;
}

static std::string resolution_token(Resolution r) {
    switch (r) {
        case Resolution::kHH: return "HH";
        case Resolution::kDD: return "DD";
        case Resolution::kWW: return "WW";
        case Resolution::kMM: return "MM";
        case Resolution::kYY: return "YY";
    }
    return "DD";
}

static std::string resolution_label(Resolution r) {
    switch (r) {
        case Resolution::kHH: return "按半小时";
        case Resolution::kDD: return "按日";
        case Resolution::kWW: return "按周";
        case Resolution::kMM: return "按月";
        case Resolution::kYY: return "按年";
    }
    return "按日";
}

static bool parse_resolution(const std::string& raw, Resolution* out) {
    const std::string s = normalize_excel_cell(raw);
    std::string up;
    up.reserve(s.size());
    for (unsigned char c : s) up.push_back(static_cast<char>(std::toupper(c)));
    if (s == "按半小时" || s == "半小时" || up == "HH") {
        *out = Resolution::kHH;
        return true;
    }
    if (s == "按日" || s == "日" || up == "DD") {
        *out = Resolution::kDD;
        return true;
    }
    if (s == "按周" || s == "周" || up == "WW") {
        *out = Resolution::kWW;
        return true;
    }
    if (s == "按月" || s == "月" || up == "MM") {
        *out = Resolution::kMM;
        return true;
    }
    if (s == "按年" || s == "年" || up == "YY") {
        *out = Resolution::kYY;
        return true;
    }
    return false;
}

static bool parse_dataset_timestamp(const std::string& raw,
                                    Resolution r,
                                    long long* scalar,
                                    std::string* label) {
    const std::string s = trim(raw);
    std::size_t need = 0;
    switch (r) {
        case Resolution::kHH: need = 12; break;
        case Resolution::kDD:
        case Resolution::kWW: need = 8; break;
        case Resolution::kMM: need = 6; break;
        case Resolution::kYY: need = 4; break;
    }
    if (s.size() != need || !all_digits(s)) return false;

    CivilTime t;
    try {
        t.year = std::stoi(s.substr(0, 4));
        if (need >= 6) t.month = std::stoi(s.substr(4, 2));
        if (need >= 8) t.day = std::stoi(s.substr(6, 2));
        if (need >= 12) {
            t.hour = std::stoi(s.substr(8, 2));
            t.minute = std::stoi(s.substr(10, 2));
        }
    } catch (...) {
        return false;
    }
    if (!valid_dataset_time(t)) return false;
    if (scalar) *scalar = scalar_time(t);
    if (label) *label = display_time(t);
    return true;
}

static bool time_in_range(long long t, const ExperimentCase& c) {
    if (c.start_time && t < *c.start_time) return false;
    if (c.end_time && t > *c.end_time) return false;
    return true;
}

static std::vector<std::string> parse_target_ids(std::string s) {
    s = normalize_excel_cell(s);
    replace_all(&s, "{", " ");
    replace_all(&s, "}", " ");
    replace_all(&s, "[", " ");
    replace_all(&s, "]", " ");
    replace_all(&s, ",", " ");
    replace_all(&s, ";", " ");
    replace_all(&s, "，", " ");
    replace_all(&s, "；", " ");

    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    std::istringstream iss(s);
    std::string id;
    while (iss >> id) {
        id = trim(id);
        if (!id.empty() && seen.insert(id).second) out.push_back(id);
    }
    return out;
}

static std::vector<std::string> parse_columns(std::string s) {
    s = normalize_excel_cell(s);
    replace_all(&s, "，", ",");
    replace_all(&s, ";", ",");
    replace_all(&s, "；", ",");
    const std::set<std::string> allowed = {"R","N","T","V","H","L","P","G"};

    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (std::string x : split_char(s, ',')) {
        x = trim(x);
        std::string up;
        up.reserve(x.size());
        for (unsigned char c : x) up.push_back(static_cast<char>(std::toupper(c)));
        if (up.empty()) continue;
        if (!allowed.count(up)) {
            throw std::runtime_error("参与列只能从 R,N,T,V,H,L,P,G 中选择，发现: " + x);
        }
        if (!seen.insert(up).second) {
            throw std::runtime_error("参与列重复: " + up);
        }
        out.push_back(up);
    }
    if (out.empty()) throw std::runtime_error("参与列不能为空");
    return out;
}

static std::string join(const std::vector<std::string>& xs, const std::string& sep) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (i) oss << sep;
        oss << xs[i];
    }
    return oss.str();
}

static const std::map<std::string, std::string>& flux_column_map() {
    static const std::map<std::string, std::string> kMap = {
        {"R", "SW_IN_F_MDS"},
        {"N", "NEE_VUT_USTAR50"},
        {"T", "TA_F_MDS"},
        {"V", "VPD_F_MDS"},
        {"H", "H_F_MDS"},
        {"L", "LE_F_MDS"},
        {"P", "P_F"},
        {"G", "GPP_NT_VUT_USTAR50"},
    };
    return kMap;
}

static ExperimentPaths resolve_paths() {
    ExperimentPaths p;
    p.root = fs::current_path();
    p.input_csv = p.root / "experiment" / "outdb_fluxnet_experiment_input.csv";
    p.db_dir = p.root / "rdb_fluxnet_outdb_experiment";
    p.log_dir = p.root / "experiment_log";
    p.summary_csv = p.log_dir / "对生物圈大气通量的入库+库外推断实验统计.csv";
    p.print_log = p.log_dir / "生物圈大气通量入库+库外推断实验全部打印日志.txt";

    if (const char* env = std::getenv("FLUXNET_DATA_DIR")) {
        if (*env) p.data_dir = fs::path(env);
    }
    if (const char* env = std::getenv("OUTDB_FLUXNET_EXPERIMENT_INPUT")) {
        if (*env) p.input_csv = fs::path(env);
    }
    if (p.data_dir.empty()) {
        const std::vector<fs::path> candidates = {
            p.root / ".." / "remixdb" / "data" / "fluxnet_data",
            p.root / "data" / "fluxnet_data",
            p.root / ".." / "remixdb" / "data" / "fluxnet_data"
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

static std::optional<Resolution> resolution_from_filename(const std::string& name) {
    for (Resolution r : {Resolution::kHH, Resolution::kDD, Resolution::kWW,
                         Resolution::kMM, Resolution::kYY}) {
        if (name.find("_" + resolution_token(r) + "_") != std::string::npos) return r;
    }
    return std::nullopt;
}

static std::string site_id_from_dataset_id(const std::string& dataset_id) {
    const std::string prefix = "FLX_";
    if (dataset_id.rfind(prefix, 0) != 0) return std::string();
    const std::size_t p = dataset_id.find('_', prefix.size());
    if (p == std::string::npos || p == prefix.size()) return std::string();
    return dataset_id.substr(prefix.size(), p - prefix.size());
}

static DiscoveredData discover_data(const fs::path& root) {
    if (root.empty() || !fs::is_directory(root)) {
        throw std::runtime_error("找不到 fluxnet_data 目录；可设置 FLUXNET_DATA_DIR。当前目录: " +
                                 root.string());
    }

    DiscoveredData d;
    for (const auto& ent : fs::recursive_directory_iterator(root)) {
        if (!ent.is_regular_file()) continue;
        const std::string name = ent.path().filename().string();
        if (name.size() < 9 || name.substr(name.size() - 4) != ".csv") continue;
        if (name.size() <= 8 || name.substr(name.size() - 8) != "_pre.csv") continue;

        const auto ropt = resolution_from_filename(name);
        if (!ropt) continue;
        const std::string dataset_id = name.substr(0, name.size() - 8);
        const std::string site_id = site_id_from_dataset_id(dataset_id);
        if (site_id.empty()) continue;
        // ZA-Kru 属于 FLUXNET2015 Tier Two，不纳入本项目公开数据范围。
        if (site_id == "ZA-Kru") continue;

        SiteData& site = d.sites[site_id];
        site.site_id = site_id;
        if (site.folder_name.empty()) {
            const std::string parent = ent.path().parent_path().filename().string();
            site.folder_name = parent.empty() ? dataset_id : parent;
        }

        ResolutionFile& rf = site.by_resolution[*ropt];
        if (!rf.path.empty() && rf.path != ent.path()) {
            throw std::runtime_error("同一站点/分辨率发现多个 *_pre.csv，无法唯一选择: " +
                                     site_id + " / " + resolution_token(*ropt));
        }
        rf.dataset_id = dataset_id;
        rf.path = ent.path();
    }

    for (auto it = d.sites.begin(); it != d.sites.end();) {
        if (it->second.by_resolution.empty()) it = d.sites.erase(it);
        else ++it;
    }
    if (d.sites.empty()) throw std::runtime_error("fluxnet_data 中没有发现可用的 *_pre.csv");

    for (const auto& kv : d.sites) d.sorted_site_ids.push_back(kv.first);
    std::sort(d.sorted_site_ids.begin(), d.sorted_site_ids.end(),
              [&](const std::string& a, const std::string& b) {
                  const SiteData& sa = d.sites.at(a);
                  const SiteData& sb = d.sites.at(b);
                  if (sa.folder_name != sb.folder_name) return sa.folder_name < sb.folder_name;
                  return a < b;
              });
    return d;
}

static const ResolutionFile* resolution_file(const DiscoveredData& d,
                                             const std::string& site_id,
                                             Resolution r) {
    auto sit = d.sites.find(site_id);
    if (sit == d.sites.end()) return nullptr;
    auto rit = sit->second.by_resolution.find(r);
    if (rit == sit->second.by_resolution.end()) return nullptr;
    return &rit->second;
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

        std::vector<std::string> f = parse_csv_line(line);
        while (!f.empty() && trim(f.back()).empty()) f.pop_back();
        if (f.size() != 8 && f.size() != 9) {
            throw std::runtime_error(
                    "outdb_fluxnet_experiment_input.csv 第 " + std::to_string(line_no) +
                    " 行应为 8 列（当前实际参数）或 9 列（兼容含“合并时的θ”的旧表头），实际=" +
                    std::to_string(f.size()));
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
            c.target_ids = parse_target_ids(target);
            if (c.target_ids.empty()) {
                throw std::runtime_error("第 " + std::to_string(line_no) + " 行指定试验品集合为空");
            }
        }

        std::string err;
        if (!parse_time_range(f[1], &c.time_range_text, &c.start_time, &c.end_time, &err)) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行: " + err);
        }
        if (!parse_resolution(f[2], &c.resolution)) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行分辨率无法识别: " + f[2]);
        }
        c.columns = parse_columns(f[3]);

        try {
            c.window_length = std::stoi(normalize_excel_cell(f[4]));
            c.tau = std::stoi(normalize_excel_cell(f[5]));
            if (f.size() == 9) {
                c.compatibility_merge_theta = std::stoi(normalize_excel_cell(f[6]));
                c.memtable_mb = std::stoi(normalize_excel_cell(f[7]));
            } else {
                c.memtable_mb = std::stoi(normalize_excel_cell(f[6]));
            }
        } catch (const std::exception&) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行存在无法解析的整数参数");
        }

        const std::size_t bool_col = f.size() == 9 ? 8 : 7;
        if (!parse_yes_no(f[bool_col], &c.use_covmeta)) {
            throw std::runtime_error("第 " + std::to_string(line_no) +
                                     " 行“是否使用协方差缓存驱动CDiT”无法识别");
        }
        if (c.window_length <= 0 || c.tau <= 0 || c.memtable_mb <= 0) {
            throw std::runtime_error("第 " + std::to_string(line_no) +
                                     " 行 T、τ、MemTable(MB) 必须为正整数");
        }
        if (c.window_length < 2 * c.tau) {
            throw std::runtime_error("第 " + std::to_string(line_no) +
                                     " 行 T < 2*τ，CDiT 无有效样本");
        }
        if (c.compatibility_merge_theta && *c.compatibility_merge_theta < 0) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行合并时的θ不能为负数");
        }
        cases.push_back(std::move(c));
    }

    if (cases.empty()) throw std::runtime_error("实验输入文件没有有效实验行");
    return cases;
}

static std::map<std::string, int> header_index(const std::vector<std::string>& header) {
    std::map<std::string, int> out;
    for (std::size_t i = 0; i < header.size(); ++i) {
        out[trim(header[i])] = static_cast<int>(i);
    }
    return out;
}

static std::size_t count_rows_in_range(const fs::path& path,
                                       Resolution r,
                                       const ExperimentCase& c,
                                       std::size_t stop_after) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开数据集: " + path.string());
    std::string line;
    if (!std::getline(in, line)) return 0;
    const auto idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    auto tit = idx.find("TIMESTAMP");
    if (tit == idx.end()) throw std::runtime_error("数据集缺少 TIMESTAMP 列: " + path.string());

    std::size_t count = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto f = parse_csv_line(line);
        if (tit->second >= static_cast<int>(f.size())) continue;
        long long scalar = 0;
        if (!parse_dataset_timestamp(f[static_cast<std::size_t>(tit->second)], r, &scalar, nullptr)) {
            throw std::runtime_error("TIMESTAMP 与分辨率不匹配: " + path.string() +
                                     " / " + f[static_cast<std::size_t>(tit->second)]);
        }
        if (!time_in_range(scalar, c)) continue;
        ++count;
        if (count >= stop_after) break;
    }
    return count;
}

static bool site_usable(const DiscoveredData& d,
                        const std::string& site_id,
                        const ExperimentCase& c) {
    const ResolutionFile* rf = resolution_file(d, site_id, c.resolution);
    if (!rf) return false;
    return count_rows_in_range(rf->path, c.resolution, c,
                               static_cast<std::size_t>(c.window_length)) >=
           static_cast<std::size_t>(c.window_length);
}

static std::vector<std::size_t> uniform_indices(std::size_t total, int n) {
    if (n <= 0 || total == 0 || static_cast<std::size_t>(n) > total) return {};
    std::vector<std::size_t> out;
    out.reserve(static_cast<std::size_t>(n));
    if (n == 1) {
        out.push_back(total / 2);
        return out;
    }
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) * static_cast<double>(total - 1) /
                         static_cast<double>(n - 1);
        out.push_back(static_cast<std::size_t>(std::llround(x)));
    }
    return out;
}

static std::vector<std::string> choose_dataset_ids(const DiscoveredData& d,
                                                   const ExperimentCase& c) {
    if (!c.target_is_count) {
        std::vector<std::string> out;
        out.reserve(c.target_ids.size());
        for (const std::string& id : c.target_ids) {
            if (!d.sites.count(id)) throw std::runtime_error("指定站点不存在: " + id);
            if (!site_usable(d, id, c)) {
                throw std::runtime_error("指定站点不满足本次分辨率/时间范围/T要求: " + id);
            }
            out.push_back(id);
        }
        return out;
    }

    if (c.target_count > static_cast<int>(d.sorted_site_ids.size())) {
        throw std::runtime_error("要求试验品个数 " + std::to_string(c.target_count) +
                                 " 超过可发现站点数 " + std::to_string(d.sorted_site_ids.size()));
    }

    std::vector<std::size_t> eligible;
    eligible.reserve(d.sorted_site_ids.size());
    for (std::size_t i = 0; i < d.sorted_site_ids.size(); ++i) {
        if (site_usable(d, d.sorted_site_ids[i], c)) eligible.push_back(i);
    }
    if (eligible.size() < static_cast<std::size_t>(c.target_count)) {
        throw std::runtime_error("满足本次分辨率/时间范围/T要求的站点只有 " +
                                 std::to_string(eligible.size()) + " 个，无法选择 " +
                                 std::to_string(c.target_count) + " 个");
    }

    // 先在所有站点文件夹排序结果上均匀取目标位置；目标不可用时，用最近的可用站点补齐。
    const auto initial = uniform_indices(d.sorted_site_ids.size(), c.target_count);
    std::set<std::size_t> selected;
    std::vector<std::size_t> missing;
    for (std::size_t idx : initial) {
        if (std::binary_search(eligible.begin(), eligible.end(), idx)) selected.insert(idx);
        else missing.push_back(idx);
    }

    for (std::size_t target : missing) {
        std::size_t best = std::numeric_limits<std::size_t>::max();
        std::size_t best_dist = std::numeric_limits<std::size_t>::max();
        for (std::size_t idx : eligible) {
            if (selected.count(idx)) continue;
            const std::size_t dist = idx > target ? idx - target : target - idx;
            if (dist < best_dist || (dist == best_dist && idx < best)) {
                best = idx;
                best_dist = dist;
            }
        }
        if (best != std::numeric_limits<std::size_t>::max()) selected.insert(best);
    }
    for (std::size_t idx : eligible) {
        if (selected.size() >= static_cast<std::size_t>(c.target_count)) break;
        selected.insert(idx);
    }
    if (selected.size() != static_cast<std::size_t>(c.target_count)) {
        throw std::runtime_error("站点均匀抽取补足逻辑失败");
    }

    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(c.target_count));
    for (std::size_t idx : selected) out.push_back(d.sorted_site_ids[idx]);
    return out;
}

static DatasetInfo load_dataset_info(const DiscoveredData& d,
                                     const std::string& site_id,
                                     const ExperimentCase& c) {
    const ResolutionFile* rf = resolution_file(d, site_id, c.resolution);
    if (!rf) throw std::runtime_error("站点缺少所选分辨率文件: " + site_id);

    std::ifstream in(rf->path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开数据集: " + rf->path.string());
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("空数据集: " + rf->path.string());
    const auto idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    auto tit = idx.find("TIMESTAMP");
    if (tit == idx.end()) throw std::runtime_error("数据集缺少 TIMESTAMP 列: " + rf->path.string());

    DatasetInfo info;
    info.site_id = site_id;
    info.dataset_id = rf->dataset_id;
    info.path = rf->path;
    long long previous = -1;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto f = parse_csv_line(line);
        if (tit->second >= static_cast<int>(f.size())) continue;
        const std::string ts = trim(f[static_cast<std::size_t>(tit->second)]);
        long long scalar = 0;
        std::string label;
        if (!parse_dataset_timestamp(ts, c.resolution, &scalar, &label)) {
            throw std::runtime_error("TIMESTAMP 与分辨率不匹配: " + site_id + " / " + ts);
        }
        if (!time_in_range(scalar, c)) continue;
        if (previous >= 0 && scalar <= previous) {
            throw std::runtime_error("数据集时间戳未严格递增: " + site_id + " / " + ts);
        }
        previous = scalar;
        if (!info.allowed_timestamp_keys.insert(ts).second) {
            throw std::runtime_error("数据集出现重复时间戳: " + site_id + " / " + ts);
        }
        info.timestamps.push_back({ts, label});
    }

    if (info.timestamps.size() < static_cast<std::size_t>(c.window_length)) {
        throw std::runtime_error("站点 " + site_id + " 在本次时间范围内不足 T 个样本");
    }
    return info;
}

static void insert_dataset(rocksdb::DB* db,
                           const DatasetInfo& info,
                           const ExperimentCase& c) {
    std::ifstream in(info.path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开插入文件: " + info.path.string());
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("插入文件为空: " + info.path.string());
    const auto idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    auto tit = idx.find("TIMESTAMP");
    if (tit == idx.end()) throw std::runtime_error("文件缺少 TIMESTAMP 列: " + info.path.string());

    std::vector<int> value_cols;
    value_cols.reserve(c.columns.size());
    int max_col = tit->second;
    for (const std::string& short_name : c.columns) {
        const std::string& full = flux_column_map().at(short_name);
        auto it = idx.find(full);
        if (it == idx.end()) {
            throw std::runtime_error("文件缺少列 " + full + ": " + info.path.string());
        }
        value_cols.push_back(it->second);
        max_col = std::max(max_col, it->second);
    }

    rocksdb::WriteOptions wopt;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto f = parse_csv_line(line);
        if (max_col >= static_cast<int>(f.size())) continue;
        const std::string ts = trim(f[static_cast<std::size_t>(tit->second)]);
        if (!info.allowed_timestamp_keys.count(ts)) continue;

        std::ostringstream value;
        for (std::size_t i = 0; i < value_cols.size(); ++i) {
            if (i) value << ',';
            value << trim(f[static_cast<std::size_t>(value_cols[i])]);
        }

        // 使用既定最小侵入格式：key=完整数据集名,原始时间戳；value=本次参与列，顺序与 c.columns 一致。
        const std::string key = info.dataset_id + "," + ts;
        const rocksdb::Status s = db->Put(wopt, key, value.str());
        if (!s.ok()) throw std::runtime_error("RocksDB Put 失败: " + s.ToString());
    }
}

static std::vector<double> parse_db_value(const std::string& value,
                                          std::size_t expected_columns,
                                          const std::string& dataset_id,
                                          const std::string& ts) {
    std::vector<double> out;
    out.reserve(expected_columns);
    std::size_t begin = 0;
    for (;;) {
        const std::size_t p = value.find(',', begin);
        const std::string field = trim(value.substr(
                begin, p == std::string::npos ? std::string::npos : p - begin));
        try {
            std::size_t used = 0;
            const double x = std::stod(field, &used);
            if (used != field.size() || !std::isfinite(x)) throw std::runtime_error("非有限数");
            out.push_back(x);
        } catch (const std::exception&) {
            throw std::runtime_error("RocksDB value 数值解析失败: " + dataset_id +
                                     " / " + ts + " / value=" + value);
        }
        if (p == std::string::npos) break;
        begin = p + 1;
    }
    if (out.size() != expected_columns) {
        throw std::runtime_error("RocksDB value 列数=" + std::to_string(out.size()) +
                                 "，期望=" + std::to_string(expected_columns) +
                                 ": " + dataset_id + " / " + ts);
    }
    return out;
}

static void destroy_db_state(const fs::path& db_dir, const rocksdb::Options& options) {
    // RocksDB 11 的 DestroyDB 在目录完全不存在时，不同平台可能返回 NotFound 或 IOError；
    // 因此先判断目录是否存在，目录不存在本身就是“空库”。
    std::error_code exists_ec;
    const bool exists = fs::exists(db_dir, exists_ec);
    if (exists_ec) {
        throw std::runtime_error("检查数据库目录失败: " + db_dir.string() +
                                 " / " + exists_ec.message());
    }
    if (!exists) return;

    // 直接调用 RocksDB 自带完整销毁接口，清除 SST/WAL/MANIFEST/OPTIONS 等数据库状态。
    const rocksdb::Status s = rocksdb::DestroyDB(db_dir.string(), options);
    if (!s.ok() && !s.IsNotFound()) {
        throw std::runtime_error("DestroyDB 失败: " + s.ToString());
    }

    // 异常强退可能遗留不属于 RocksDB 标准集合的杂项，额外删除目录兜底。
    std::error_code ec;
    fs::remove_all(db_dir, ec);
    if (ec) {
        throw std::runtime_error("清理数据库残留目录失败: " + db_dir.string() +
                                 " / " + ec.message());
    }
}

static void destroy_db_state_noexcept(const fs::path& db_dir, const rocksdb::Options& options) {
    try {
        destroy_db_state(db_dir, options);
    } catch (...) {
        // 异常传播路径只做 best-effort 清理，不覆盖最先出现的实验异常。
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
        // 与已经提供的 outdb_basin_experiment 保持一致：
        // 只合并时间上相邻、且完整因果图（包括每条边的滞后集合）完全相同的窗口。
        segments->back().end_label = end_label;
        return;
    }
    segments->push_back({start_label, end_label, std::move(graph)});
}

static std::string format_graph(const CDiTResult& graph,
                                const std::vector<std::string>& columns) {
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
        if (x >= 1 && x <= static_cast<int>(columns.size())) {
            return columns[static_cast<std::size_t>(x - 1)];
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

static std::vector<InferenceSegment> infer_one_dataset_from_rocksdb(
        rocksdb::DB* db,
        const DatasetInfo& info,
        const ExperimentCase& c,
        QueryTiming* timing) {
    const std::size_t T = static_cast<std::size_t>(c.window_length);
    const std::size_t complete_window_count = info.timestamps.size() / T;

    std::vector<InferenceSegment> segments;
    segments.reserve(complete_window_count);
    const rocksdb::ReadOptions ropt;

    for (std::size_t w = 0; w < complete_window_count; ++w) {
        const std::size_t begin = w * T;
        const std::size_t end = begin + T;
        std::vector<std::vector<double>> data;
        data.reserve(T);

        // 读取计时只覆盖 RocksDB 公共 DB::Get + value 数值解析；
        // 每个完整窗口读取完毕后立即推断，再去读下一个窗口。
        const Clock::time_point read0 = Clock::now();
        for (std::size_t i = begin; i < end; ++i) {
            const TimestampInfo& ti = info.timestamps[i];
            const std::string key = info.dataset_id + "," + ti.key;
            std::string value;
            const rocksdb::Status s = db->Get(ropt, key, &value);
            if (!s.ok()) {
                throw std::runtime_error("RocksDB DB::Get 读取失败: key=" + key +
                                         " / " + s.ToString());
            }
            data.push_back(parse_db_value(value, c.columns.size(), info.dataset_id, ti.key));
        }
        timing->read_seconds += seconds_between(read0, Clock::now());

        // 推断计时只覆盖 CDiT/LSS-CDiT 本体；忽略自连接。
        const Clock::time_point infer0 = Clock::now();
        CDiTResult graph = c.use_covmeta
                ? run_cdit_covmeta(data, c.tau, 0.01, false, nullptr, nullptr)
                : run_cdit_original(data, c.tau, 0.01, false, nullptr, nullptr);
        timing->inference_seconds += seconds_between(infer0, Clock::now());

        append_or_merge_segment(&segments,
                                info.timestamps[begin].label,
                                info.timestamps[end - 1].label,
                                std::move(graph));
    }
    return segments;
}

static std::string summary_target_field(const ExperimentCase& c,
                                        const std::vector<std::string>& site_ids) {
    if (c.target_is_count) return std::to_string(site_ids.size());
    return join(site_ids, " ");
}

static void initialize_summary_csv(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("无法创建实验统计 CSV: " + path.string());
    // UTF-8 BOM：Windows Excel 直接双击时不会按本地 ANSI/GBK 误判中文编码。
    out.write("\xEF\xBB\xBF", 3);
    out << "试验品个数或指定试验品集合,查询时间起止范围,选择分辨率,参与列,完整子随机过程长度T,推断时的τ,MemTable容量(MB),是否使用协方差缓存驱动CDiT,总耗时(s),插入总耗时(s),库外推断中仅读取部分耗时(s),库外推断中仅推断部分耗时(s)\n";
    out.flush();
}

static void append_summary_row(const fs::path& path,
                               const ExperimentCase& c,
                               const std::vector<std::string>& site_ids,
                               double total_seconds,
                               double insert_seconds,
                               double read_seconds,
                               double inference_seconds) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("无法追加实验统计 CSV: " + path.string());

    const std::vector<std::string> fields = {
        summary_target_field(c, site_ids),
        excel_safe_time_range(c.time_range_text),
        resolution_label(c.resolution),
        join(c.columns, ","),
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
    // 每完成一次实验立即写入并刷新一行，不等全部实验结束后再批量生成。
    out.flush();
}

static QueryTiming run_outdb_inference(rocksdb::DB* db,
                                       const std::map<std::string, DatasetInfo>& infos,
                                       const std::vector<std::string>& site_ids,
                                       const ExperimentCase& c,
                                       ExperimentLogger& logger) {
    QueryTiming timing;

    logger.line("本次范围推断输入：试验品个数=" + std::to_string(site_ids.size()) +
                "；因果查询起止=-∞到+∞；分辨率=" + resolution_label(c.resolution) +
                "；参与列=" + join(c.columns, ",") +
                "；采样步长=1；T=" + std::to_string(c.window_length) +
                "；τ=" + std::to_string(c.tau) + "；");

    for (const std::string& site_id : site_ids) {
        const DatasetInfo& info = infos.at(site_id);
        logger.line("试验品：" + info.dataset_id);

        // “本次范围查询小节”计时覆盖读取、推断和内部窗口组织，
        // 但不把后续控制台/日志打印 I/O 混入算法耗时。
        const Clock::time_point one0 = Clock::now();
        const std::vector<InferenceSegment> segments =
                infer_one_dataset_from_rocksdb(db, info, c, &timing);
        timing.total_seconds += seconds_between(one0, Clock::now());

        for (const InferenceSegment& seg : segments) {
            logger.line(seg.start_label + "至" + seg.end_label + " | 推断：" +
                        format_graph(seg.graph, c.columns));
        }
    }

    std::ostringstream oss;
    oss << "本次范围查询小节：耗时:" << std::fixed << std::setprecision(4)
        << timing.total_seconds << "s，";
    logger.line(oss.str());
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
                               const DiscoveredData& discovered,
                               const ExperimentCase& c,
                               const std::vector<std::string>& site_ids,
                               int experiment_index,
                               ExperimentLogger& logger) {
    rocksdb::Options options = make_options(c);

    // 第一次/每次实验正式计时前都保证为空库；清理时间不进入任何实验计时。
    destroy_db_state(paths.db_dir, options);

    // 本次“总耗时”从正式实验开始直到库外推断和数据库关闭完成；最终清库不计入。
    const Clock::time_point total0 = Clock::now();

    // 本实验明确关闭 RocksDB 写路径上的库内 causal_meta 元数据维护。
    causal_meta_reset_all();
    causal_meta_set_enabled_cpp(false);
    std::string attach_error;
    if (!causal_meta_attach_to_rocksdb_options(&options, &attach_error)) {
        throw std::runtime_error(attach_error);
    }

    std::map<std::string, DatasetInfo> infos;
    for (const std::string& site_id : site_ids) {
        infos.emplace(site_id, load_dataset_info(discovered, site_id, c));
    }

    logger.line("生物圈大气通量第" + std::to_string(experiment_index) + "次实验");
    logger.line("数据库参数设置：MemTable=" + std::to_string(c.memtable_mb) + "MB");
    logger.line("本此插入实验参数设置：插入时间范围=" + c.time_range_text +
                "；要插入的试验品个数=" + std::to_string(site_ids.size()) +
                "；分辨率=" + resolution_label(c.resolution) +
                "；是否使用协方差缓存驱动CDiT：" + yesno(c.use_covmeta) + "；");
    logger.line();

    std::unique_ptr<rocksdb::DB> db;
    try {
        rocksdb::Status s = rocksdb::DB::Open(options, paths.db_dir.string(), &db);
        if (!s.ok()) throw std::runtime_error("RocksDB Open 失败: " + s.ToString());

        const Clock::time_point insert0 = Clock::now();
        for (const std::string& site_id : site_ids) {
            insert_dataset(db.get(), infos.at(site_id), c);
        }

        // 显式把最后一个尚未达到 MemTable 阈值的 mutable MemTable 也同步 Flush 到 SST。
        rocksdb::FlushOptions fopt;
        fopt.wait = true;
        s = db->Flush(fopt);
        if (!s.ok()) throw std::runtime_error("RocksDB Flush 失败: " + s.ToString());

        // 关闭插入实例；后续必须重新 Open，再从 RocksDB 公共读取接口取数。
        db.reset();
        const double insert_seconds = seconds_between(insert0, Clock::now());

        rocksdb::Options read_options = options;
        read_options.create_if_missing = false;
        s = rocksdb::DB::Open(read_options, paths.db_dir.string(), &db);
        if (!s.ok()) throw std::runtime_error("RocksDB 重新打开失败: " + s.ToString());

        const QueryTiming query_timing =
                run_outdb_inference(db.get(), infos, site_ids, c, logger);
        db.reset();

        const double total_seconds = seconds_between(total0, Clock::now());
        append_summary_row(paths.summary_csv, c, site_ids,
                           total_seconds, insert_seconds,
                           query_timing.read_seconds, query_timing.inference_seconds);
    } catch (...) {
        db.reset();
        destroy_db_state_noexcept(paths.db_dir, options);
        throw;
    }

    // 每次实验完成后恢复为空库；这个清理时间不进入总耗时/插入耗时/读取耗时/推断耗时。
    destroy_db_state(paths.db_dir, options);
}

}  // 匿名命名空间结束

int run_outdb_fluxnet_experiments() {
    try {
        const ExperimentPaths paths = resolve_paths();
        if (paths.data_dir.empty()) {
            throw std::runtime_error(
                    "未找到生物圈大气通量数据目录。请 export FLUXNET_DATA_DIR=../remixdb/data/fluxnet_data");
        }

        const DiscoveredData discovered = discover_data(paths.data_dir);
        const std::vector<ExperimentCase> cases = load_cases(paths.input_csv);

        ExperimentLogger logger(paths.print_log);
        logger.initialize();
        initialize_summary_csv(paths.summary_csv);

        // 即使上一轮强退，也在第 1 次实验开始前清空 DB/WAL/SST/MANIFEST；不计时。
        rocksdb::Options cleanup_options;
        cleanup_options.create_if_missing = true;
        destroy_db_state(paths.db_dir, cleanup_options);

        int experiment_index = 0;
        for (const ExperimentCase& c : cases) {
            const std::vector<std::string> site_ids = choose_dataset_ids(discovered, c);
            ++experiment_index;
            run_one_experiment(paths, discovered, c, site_ids, experiment_index, logger);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "生物圈大气通量入库+库外推断实验失败: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "生物圈大气通量入库+库外推断实验失败: 未知异常" << std::endl;
        return 2;
    }
}
