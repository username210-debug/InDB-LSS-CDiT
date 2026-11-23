/*
 * 本文件实现 Fluxnet 数据集在 RocksDB 中的实验流程，包括站点数据准备、写入、元数据维护、查询和统计。
 */

#include "experiment/fluxnet_experiment.h"

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

enum class Resolution {
    kHH,
    kDD,
    kWW,
    kMM,
    kYY,
};

struct ResolutionFiles {
    std::string dataset_id;  // 去掉 _pre(.csv/_severity.csv) 后的完整数据集名。
    fs::path clean;
    std::map<std::string, fs::path> faults;
};

struct SiteData {
    std::string site_id;      // 例如 AT-Neu。
    std::string folder_name;  // 用于“全部站点文件夹按名称排序后均匀取样”。
    std::map<Resolution, ResolutionFiles> by_resolution;
};

struct DiscoveredData {
    std::map<std::string, SiteData> sites;  // 键由 site_id 构成。
    std::vector<std::string> sorted_site_ids;
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
    bool test_fault = false;
    bool print_process_log = false;
    bool use_covmeta = false;
};

struct DatasetBaseline {
    std::string site_id;
    std::string dataset_id;
    fs::path clean_path;
    std::vector<CausalMetaTimestampInfo> timestamps;
    std::unordered_set<std::string> allowed_timestamp_keys;
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
        // UTF-8 BOM：Windows/Excel/记事本双击时更稳定识别中文。
        log_stream_.write("\xEF\xBB\xBF", 3);
        log_stream_.flush();
    }

    void line(const std::string& text = std::string()) {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << text << '\n';
        if (log_stream_) {
            log_stream_ << text << '\n';
            // 每行刷新，长实验中可随时打开查看已经完成的历史输出。
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
    // parse_csv_line 已移除 CSV 外层引号；这里处理 Excel 公式 ="..."。
    if (s.size() >= 3 && s[0] == '=' && s[1] == '"' && s.back() == '"') {
        s = s.substr(2, s.size() - 3);
    }
    // 同时容忍手工编辑后仍残留的一层普通引号。
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return trim(s);
}

static std::string excel_safe_time_range(const std::string& s) {
    if (s == "-∞到+∞") return "=\"-∞到+∞\"";
    return s;
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

static bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    return true;
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

static bool valid_time(const CivilTime& t) {
    if (t.year < 1 || t.month < 1 || t.month > 12) return false;
    if (t.day < 1 || t.day > days_in_month(t.year, t.month)) return false;
    if (t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59) return false;
    return true;
}

static long long scalar_time(const CivilTime& t) {
    return static_cast<long long>(t.year) * 100000000LL +
           static_cast<long long>(t.month) * 1000000LL +
           static_cast<long long>(t.day) * 10000LL +
           static_cast<long long>(t.hour) * 100LL +
           t.minute;
}

static std::string display_time(const CivilTime& t) {
    std::ostringstream oss;
    oss << t.year << '-' << t.month << '-' << t.day << ' ' << t.hour << ':' << t.minute;
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
    if (d.size() != 3 || !all_digits(trim(d[0])) || !all_digits(trim(d[1])) || !all_digits(trim(d[2]))) {
        return false;
    }
    CivilTime t;
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
    if (!valid_time(t)) return false;
    *out = t;
    return true;
}

static bool parse_time_range(const std::string& raw,
                             std::string* normalized,
                             std::optional<long long>* start,
                             std::optional<long long>* end,
                             std::string* error) {
    std::string s = normalize_excel_cell(raw);
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
    CivilTime a, b;
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
    std::string s = normalize_excel_cell(raw);
    std::string up;
    up.reserve(s.size());
    for (unsigned char c : s) up.push_back(static_cast<char>(std::toupper(c)));
    if (s == "按半小时" || s == "半小时" || up == "HH") { *out = Resolution::kHH; return true; }
    if (s == "按日" || s == "日" || up == "DD") { *out = Resolution::kDD; return true; }
    if (s == "按周" || s == "周" || up == "WW") { *out = Resolution::kWW; return true; }
    if (s == "按月" || s == "月" || up == "MM") { *out = Resolution::kMM; return true; }
    if (s == "按年" || s == "年" || up == "YY") { *out = Resolution::kYY; return true; }
    return false;
}

static bool parse_dataset_timestamp(const std::string& raw,
                                    Resolution r,
                                    CivilTime* out,
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
    if (!valid_time(t)) return false;
    if (out) *out = t;
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
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    std::string x;
    while (iss >> x) {
        x = trim(x);
        if (!x.empty() && seen.insert(x).second) out.push_back(x);
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
        for (unsigned char c : x) up.push_back(static_cast<char>(std::toupper(c)));
        if (up.empty()) continue;
        if (!allowed.count(up)) throw std::runtime_error("参与列只能从 R,N,T,V,H,L,P,G 中选择，发现: " + x);
        if (!seen.insert(up).second) throw std::runtime_error("参与列重复: " + up);
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

static ExperimentPaths resolve_paths() {
    ExperimentPaths p;
    p.root = fs::current_path();
    p.input_csv = p.root / "experiment" / "fluxnet_experiment_input.csv";
    p.meta_dir = p.root / "meta_file" / "fluxnet_experiment";
    p.db_dir = p.root / "rdb_fluxnet_experiment";
    p.log_dir = p.root / "experiment_log";
    p.summary_csv = p.log_dir / "生物圈大气通量全部实验小结.csv";
    p.print_log = p.log_dir / "生物圈大气通量全部实验打印日志.txt";

    if (const char* env = std::getenv("FLUXNET_DATA_DIR")) {
        if (*env) p.data_dir = fs::path(env);
    }
    if (const char* env = std::getenv("FLUXNET_EXPERIMENT_INPUT")) {
        if (*env) p.input_csv = fs::path(env);
    }
    if (p.data_dir.empty()) {
        const std::vector<fs::path> candidates = {
            p.root / ".." / "remixdb" / "data" / "fluxnet_data",
            p.root / "data" / "fluxnet_data",
            p.root / ".." / "remixdb" / "data" / "fluxnet_data"
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

static std::optional<Resolution> resolution_from_filename(const std::string& name) {
    for (Resolution r : {Resolution::kHH, Resolution::kDD, Resolution::kWW, Resolution::kMM, Resolution::kYY}) {
        const std::string marker = "_" + resolution_token(r) + "_";
        if (name.find(marker) != std::string::npos) return r;
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
        throw std::runtime_error("找不到 fluxnet_data 目录；可设置 FLUXNET_DATA_DIR。当前目录: " + root.string());
    }

    DiscoveredData d;
    for (const auto& ent : fs::recursive_directory_iterator(root)) {
        if (!ent.is_regular_file()) continue;
        const std::string name = ent.path().filename().string();
        if (name.size() < 9 || name.substr(name.size() - 4) != ".csv") continue;
        const std::size_t pre = name.find("_pre");
        if (pre == std::string::npos) continue;
        const auto ropt = resolution_from_filename(name);
        if (!ropt) continue;

        const std::string dataset_id = name.substr(0, pre);
        const std::string site_id = site_id_from_dataset_id(dataset_id);
        if (site_id.empty()) continue;
        // ZA-Kru 属于 FLUXNET2015 Tier Two，不纳入本项目公开数据范围。
        if (site_id == "ZA-Kru") continue;

        SiteData& site = d.sites[site_id];
        site.site_id = site_id;
        if (site.folder_name.empty()) {
            const std::string parent_name = ent.path().parent_path().filename().string();
            site.folder_name = parent_name.empty() ? dataset_id : parent_name;
        }

        ResolutionFiles& rf = site.by_resolution[*ropt];
        if (rf.dataset_id.empty()) rf.dataset_id = dataset_id;
        if (rf.dataset_id != dataset_id) {
            throw std::runtime_error("同一站点/分辨率发现多个数据包版本，无法唯一选择: " + site_id + " / " + resolution_token(*ropt));
        }

        const std::string tail = name.substr(pre + 4);  // .csv 或 _severity.csv
        if (tail == ".csv") {
            if (!rf.clean.empty() && rf.clean != ent.path()) {
                throw std::runtime_error("发现重复 clean 文件: " + site_id + " / " + resolution_token(*ropt));
            }
            rf.clean = ent.path();
        } else if (tail.size() > 5 && tail.front() == '_' && tail.substr(tail.size() - 4) == ".csv") {
            const std::string sev = tail.substr(1, tail.size() - 5);
            if (!sev.empty()) rf.faults[sev] = ent.path();
        }
    }

    // 没有 clean 的站点/分辨率条目不能用于实验；fault 不能脱离同名 clean 基线单独使用。
    for (auto sit = d.sites.begin(); sit != d.sites.end();) {
        for (auto rit = sit->second.by_resolution.begin(); rit != sit->second.by_resolution.end();) {
            if (rit->second.clean.empty()) rit = sit->second.by_resolution.erase(rit);
            else ++rit;
        }
        if (sit->second.by_resolution.empty()) sit = d.sites.erase(sit);
        else ++sit;
    }
    if (d.sites.empty()) throw std::runtime_error("fluxnet_data 中没有发现可用的 *_pre.csv");

    for (const auto& kv : d.sites) d.sorted_site_ids.push_back(kv.first);
    std::sort(d.sorted_site_ids.begin(), d.sorted_site_ids.end(), [&](const std::string& a, const std::string& b) {
        const SiteData& sa = d.sites.at(a);
        const SiteData& sb = d.sites.at(b);
        if (sa.folder_name != sb.folder_name) return sa.folder_name < sb.folder_name;
        return a < b;
    });
    return d;
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
        const auto f = parse_csv_line(line);
        if (f.size() < 10) {
            throw std::runtime_error("fluxnet_experiment_input.csv 第 " + std::to_string(line_no) + " 行少于 10 列");
        }

        ExperimentCase c;
        const std::string target = normalize_excel_cell(f[0]);
        if (all_digits(target)) {
            c.target_is_count = true;
            c.target_count = std::stoi(target);
            if (c.target_count <= 0) throw std::runtime_error("第 " + std::to_string(line_no) + " 行试验品个数必须 >0");
        } else {
            c.target_ids = parse_target_ids(target);
            if (c.target_ids.empty()) throw std::runtime_error("第 " + std::to_string(line_no) + " 行指定试验品集合为空");
        }

        std::string err;
        if (!parse_time_range(f[1], &c.time_range_text, &c.start_time, &c.end_time, &err)) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行: " + err);
        }
        if (!parse_resolution(f[2], &c.resolution)) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行分辨率无法识别: " + f[2]);
        }
        c.columns = parse_columns(f[3]);
        c.window_length = std::stoi(normalize_excel_cell(f[4]));
        c.tau = std::stoi(normalize_excel_cell(f[5]));
        c.memtable_mb = std::stoi(normalize_excel_cell(f[6]));
        if (c.window_length <= 0 || c.tau <= 0 || c.memtable_mb <= 0) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行 T、τ、MemTable(MB) 必须为正整数");
        }
        if (c.window_length < 2 * c.tau) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行 T < 2*τ，CDiT 无有效样本");
        }
        if (!parse_yes_no(f[7], &c.test_fault) ||
            !parse_yes_no(f[8], &c.print_process_log) ||
            !parse_yes_no(f[9], &c.use_covmeta)) {
            throw std::runtime_error("第 " + std::to_string(line_no) + " 行的是/否字段无法识别");
        }
        out.push_back(std::move(c));
    }
    if (out.empty()) throw std::runtime_error("实验输入文件没有有效实验行");
    return out;
}

static const ResolutionFiles* resolution_files(const DiscoveredData& d,
                                               const std::string& site_id,
                                               Resolution r) {
    auto sit = d.sites.find(site_id);
    if (sit == d.sites.end()) return nullptr;
    auto rit = sit->second.by_resolution.find(r);
    if (rit == sit->second.by_resolution.end()) return nullptr;
    return &rit->second;
}

static std::size_t count_clean_rows_in_range(const fs::path& path,
                                             Resolution r,
                                             const ExperimentCase& c,
                                             std::size_t stop_after) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开 clean 数据集: " + path.string());
    std::string line;
    if (!std::getline(in, line)) return 0;
    const auto h = parse_csv_line(remove_utf8_bom(line));
    int time_col = -1;
    for (std::size_t i = 0; i < h.size(); ++i) {
        if (trim(h[i]) == "TIMESTAMP") {
            time_col = static_cast<int>(i);
            break;
        }
    }
    if (time_col < 0) throw std::runtime_error("数据集缺少 TIMESTAMP 列: " + path.string());

    std::size_t count = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto f = parse_csv_line(line);
        if (time_col >= static_cast<int>(f.size())) continue;
        long long t = 0;
        if (!parse_dataset_timestamp(f[static_cast<std::size_t>(time_col)], r, nullptr, &t, nullptr)) continue;
        if (!time_in_range(t, c)) continue;
        ++count;
        if (count >= stop_after) break;
    }
    return count;
}

static std::unordered_map<std::string, bool> build_usable_site_map(const DiscoveredData& d,
                                                                   const ExperimentCase& c) {
    std::unordered_map<std::string, bool> usable;
    const std::size_t need = static_cast<std::size_t>(c.window_length);
    for (const std::string& id : d.sorted_site_ids) {
        const ResolutionFiles* rf = resolution_files(d, id, c.resolution);
        if (!rf) {
            usable[id] = false;
            continue;
        }
        // 至少拥有 T 个本次时间范围内的 clean 时间戳，才能形成一个完整随机过程窗口。
        usable[id] = count_clean_rows_in_range(rf->clean, c.resolution, c, need) >= need;
    }
    return usable;
}

static bool site_available_for(const DiscoveredData& d,
                               const std::unordered_map<std::string, bool>& usable,
                               const std::string& site_id,
                               Resolution r,
                               const std::string& severity) {
    auto uit = usable.find(site_id);
    if (uit == usable.end() || !uit->second) return false;
    const ResolutionFiles* rf = resolution_files(d, site_id, r);
    if (!rf) return false;
    if (severity.empty()) return true;
    return rf->faults.count(severity) != 0;
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

static std::vector<std::string> choose_count_sites(const DiscoveredData& d,
                                                   const ExperimentCase& c,
                                                   const std::unordered_map<std::string, bool>& usable,
                                                   const std::string& severity) {
    const int n = c.target_count;
    if (n > static_cast<int>(d.sorted_site_ids.size())) {
        throw std::runtime_error("要求试验品个数 " + std::to_string(n) +
                                 " 超过全部站点文件夹数 " + std::to_string(d.sorted_site_ids.size()));
    }

    std::vector<std::size_t> eligible_indices;
    for (std::size_t i = 0; i < d.sorted_site_ids.size(); ++i) {
        if (site_available_for(d, usable, d.sorted_site_ids[i], c.resolution, severity)) {
            eligible_indices.push_back(i);
        }
    }
    if (eligible_indices.size() < static_cast<std::size_t>(n)) {
        throw std::runtime_error("满足分辨率/文件后缀/时间范围/T要求的站点仅 " +
                                 std::to_string(eligible_indices.size()) + " 个，无法补足到 " + std::to_string(n));
    }

    // 严格按用户要求：先在“全部站点文件夹排序”上均匀取 n 个，再过滤不可用项；
    // 对每个缺口选择离原目标位置最近的可用站点补齐，尽量保持原本的均匀覆盖。
    const auto initial = uniform_indices(d.sorted_site_ids.size(), n);
    std::set<std::size_t> selected;
    std::vector<std::size_t> missing_targets;
    for (std::size_t idx : initial) {
        const std::string& id = d.sorted_site_ids[idx];
        if (site_available_for(d, usable, id, c.resolution, severity)) selected.insert(idx);
        else missing_targets.push_back(idx);
    }

    for (std::size_t target : missing_targets) {
        std::size_t best = std::numeric_limits<std::size_t>::max();
        std::size_t best_dist = std::numeric_limits<std::size_t>::max();
        for (std::size_t idx : eligible_indices) {
            if (selected.count(idx)) continue;
            const std::size_t dist = idx > target ? idx - target : target - idx;
            if (dist < best_dist || (dist == best_dist && idx < best)) {
                best = idx;
                best_dist = dist;
            }
        }
        if (best != std::numeric_limits<std::size_t>::max()) selected.insert(best);
    }
    for (std::size_t idx : eligible_indices) {
        if (selected.size() >= static_cast<std::size_t>(n)) break;
        selected.insert(idx);
    }
    if (selected.size() != static_cast<std::size_t>(n)) {
        throw std::runtime_error("站点补足逻辑未能得到要求数量");
    }

    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(n));
    for (std::size_t idx : selected) out.push_back(d.sorted_site_ids[idx]);
    return out;
}

static std::vector<std::string> choose_specified_sites(const DiscoveredData& d,
                                                       const ExperimentCase& c,
                                                       const std::unordered_map<std::string, bool>& usable,
                                                       const std::string& severity) {
    std::vector<std::string> out;
    for (const std::string& id : c.target_ids) {
        if (!d.sites.count(id)) throw std::runtime_error("指定站点不存在: " + id);
        if (!site_available_for(d, usable, id, c.resolution, severity)) {
            throw std::runtime_error("指定站点不满足本次分辨率/故障程度/时间范围/T要求: " + id);
        }
        out.push_back(id);
    }
    return out;
}

static int severity_percent(const std::string& s) {
    const std::size_t p = s.find('%');
    if (p == std::string::npos) return 1000000;
    const std::string x = s.substr(0, p);
    return all_digits(x) ? std::stoi(x) : 1000000;
}

static std::vector<std::string> fault_severities_for_case(
        const DiscoveredData& d,
        const ExperimentCase& c,
        const std::unordered_map<std::string, bool>& usable) {
    std::vector<std::string> out = {""};  // 空串表示 clean。
    if (!c.test_fault) return out;

    std::set<std::string> severities;
    if (c.target_is_count) {
        std::map<std::string, int> counts;
        for (const std::string& id : d.sorted_site_ids) {
            auto uit = usable.find(id);
            if (uit == usable.end() || !uit->second) continue;
            const ResolutionFiles* rf = resolution_files(d, id, c.resolution);
            if (!rf) continue;
            for (const auto& kv : rf->faults) ++counts[kv.first];
        }
        for (const auto& kv : counts) {
            if (kv.second >= c.target_count) severities.insert(kv.first);
        }
    } else {
        bool first = true;
        for (const std::string& id : c.target_ids) {
            if (!d.sites.count(id)) throw std::runtime_error("指定站点不存在: " + id);
            auto uit = usable.find(id);
            if (uit == usable.end() || !uit->second) {
                throw std::runtime_error("指定站点在本次时间范围内不足 T 个样本: " + id);
            }
            const ResolutionFiles* rf = resolution_files(d, id, c.resolution);
            if (!rf) throw std::runtime_error("指定站点没有所选分辨率文件: " + id);
            std::set<std::string> here;
            for (const auto& kv : rf->faults) here.insert(kv.first);
            if (first) {
                severities = std::move(here);
                first = false;
            } else {
                std::set<std::string> common;
                std::set_intersection(severities.begin(), severities.end(),
                                      here.begin(), here.end(),
                                      std::inserter(common, common.begin()));
                severities.swap(common);
            }
        }
    }

    std::vector<std::string> v(severities.begin(), severities.end());
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
        const int pa = severity_percent(a), pb = severity_percent(b);
        if (pa != pb) return pa < pb;
        return a < b;
    });
    out.insert(out.end(), v.begin(), v.end());
    return out;
}

static std::map<std::string, int> header_index(const std::vector<std::string>& h) {
    std::map<std::string, int> out;
    for (std::size_t i = 0; i < h.size(); ++i) out[trim(h[i])] = static_cast<int>(i);
    return out;
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

static DatasetBaseline load_baseline(const DiscoveredData& d,
                                     const std::string& site_id,
                                     const ExperimentCase& c) {
    const ResolutionFiles* rf = resolution_files(d, site_id, c.resolution);
    if (!rf) throw std::runtime_error("站点缺少所选分辨率 clean 文件: " + site_id);

    std::ifstream in(rf->clean, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开 clean 数据集: " + rf->clean.string());
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("空数据集: " + rf->clean.string());
    const auto idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    auto tit = idx.find("TIMESTAMP");
    if (tit == idx.end()) throw std::runtime_error("数据集缺少 TIMESTAMP 列: " + rf->clean.string());

    DatasetBaseline b;
    b.site_id = site_id;
    b.dataset_id = rf->dataset_id;
    b.clean_path = rf->clean;
    long long previous = -1;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto f = parse_csv_line(line);
        if (tit->second >= static_cast<int>(f.size())) continue;
        const std::string ts = trim(f[static_cast<std::size_t>(tit->second)]);
        long long scalar = 0;
        std::string label;
        if (!parse_dataset_timestamp(ts, c.resolution, nullptr, &scalar, &label)) {
            throw std::runtime_error("TIMESTAMP 与分辨率不匹配: " + site_id + " / " + ts);
        }
        if (!time_in_range(scalar, c)) continue;
        if (previous >= 0 && scalar <= previous) {
            throw std::runtime_error("clean 数据集时间戳未严格递增: " + site_id + " / " + ts);
        }
        previous = scalar;
        if (!b.allowed_timestamp_keys.insert(ts).second) {
            throw std::runtime_error("clean 数据集出现重复时间戳: " + site_id + " / " + ts);
        }
        b.timestamps.push_back({ts, label});
    }
    if (b.timestamps.size() < static_cast<std::size_t>(c.window_length)) {
        throw std::runtime_error("站点 " + site_id + " 在本次时间范围内不足 T 个样本");
    }
    return b;
}

static fs::path choose_data_file(const DiscoveredData& d,
                                 const std::string& site_id,
                                 Resolution r,
                                 const std::string& severity) {
    const ResolutionFiles* rf = resolution_files(d, site_id, r);
    if (!rf) throw std::runtime_error("站点缺少所选分辨率文件: " + site_id);
    if (severity.empty()) return rf->clean;
    auto it = rf->faults.find(severity);
    if (it == rf->faults.end()) {
        throw std::runtime_error("缺少故障数据文件: " + site_id + " / " + severity);
    }
    return it->second;
}

static void insert_dataset(rocksdb::DB* db,
                           const fs::path& path,
                           const DatasetBaseline& baseline,
                           const ExperimentCase& c) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开插入文件: " + path.string());
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("插入文件为空: " + path.string());
    const auto idx = header_index(parse_csv_line(remove_utf8_bom(line)));
    auto tit = idx.find("TIMESTAMP");
    if (tit == idx.end()) throw std::runtime_error("文件缺少 TIMESTAMP 列: " + path.string());

    std::vector<int> value_cols;
    value_cols.reserve(c.columns.size());
    int max_col = tit->second;
    for (const std::string& short_name : c.columns) {
        const std::string& full = flux_column_map().at(short_name);
        auto it = idx.find(full);
        if (it == idx.end()) throw std::runtime_error("文件缺少列 " + full + ": " + path.string());
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
        if (!baseline.allowed_timestamp_keys.count(ts)) continue;

        std::ostringstream value;
        for (std::size_t i = 0; i < value_cols.size(); ++i) {
            if (i) value << ',';
            value << trim(f[static_cast<std::size_t>(value_cols[i])]);
        }
        const std::string key = baseline.dataset_id + "," + ts;
        const rocksdb::Status s = db->Put(wopt, key, value.str());
        if (!s.ok()) throw std::runtime_error("RocksDB Put 失败: " + s.ToString());
    }
}

static void destroy_db_state(const fs::path& db_dir, const rocksdb::Options& options) {
    // RocksDB 自带的完整数据库销毁接口；用于清理 SST/WAL/MANIFEST/OPTIONS 等数据库状态。
    const rocksdb::Status s = rocksdb::DestroyDB(db_dir.string(), options);
    if (!s.ok() && !s.IsNotFound()) {
        throw std::runtime_error("DestroyDB 失败: " + s.ToString());
    }
    // 对异常强退可能遗留、但不属于 RocksDB 正常文件集合的杂项再做兜底清理。
    std::error_code ec;
    fs::remove_all(db_dir, ec);
}

static std::string yesno(bool v) { return v ? "是" : "否"; }

static void initialize_summary_csv(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("无法创建实验小结 CSV: " + path.string());
    out.write("\xEF\xBB\xBF", 3);
    out << "插入的时间范围,插入的试验品的数量,分辨率,插入时的T,推断时的τ,插入的列,MemTable(MB),是否测试故障数据集,是否打印过程日志,是否使用协方差缓存驱动CDiT,插入耗时(s),全数据集全跨度全列范围查询总耗时(s),库内推断算法部分耗时(s),故障程度,全部页面数,互补页面数,重叠页面数,插入总耗时减库内推断耗时(s)\n";
    out.flush();
}

static void append_summary_row(const fs::path& path,
                               const ExperimentCase& c,
                               int dataset_count,
                               double insert_seconds,
                               double query_seconds,
                               const CausalMetaCounters& counters,
                               const std::string& severity) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("无法追加实验小结 CSV: " + path.string());
    const double derived = insert_seconds - counters.metadata_seconds;
    const std::vector<std::string> fields = {
        excel_safe_time_range(c.time_range_text),
        std::to_string(dataset_count),
        resolution_label(c.resolution),
        std::to_string(c.window_length),
        std::to_string(c.tau),
        join(c.columns, ","),
        std::to_string(c.memtable_mb),
        yesno(c.test_fault),
        yesno(c.print_process_log),
        yesno(c.use_covmeta),
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

static std::string target_description(const ExperimentCase& c,
                                      const std::vector<std::string>& ids) {
    if (c.target_is_count) return "要插入的试验品个数=" + std::to_string(ids.size());
    return "指定试验品集合={" + join(ids, ",") + "}；实际试验品个数=" + std::to_string(ids.size());
}

static std::string format_query_segment(const CausalMetaQuerySegment& seg,
                                        const std::vector<std::string>& preferred_order) {
    std::map<std::string, int> rank;
    for (std::size_t i = 0; i < preferred_order.size(); ++i) {
        rank[preferred_order[i]] = static_cast<int>(i);
    }
    auto name_of = [&](int idx) -> std::string {
        if (idx >= 1 && idx <= static_cast<int>(seg.variable_names.size())) {
            return seg.variable_names[static_cast<std::size_t>(idx - 1)];
        }
        return "X" + std::to_string(idx);
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
    for (auto item : items) {
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
    double total_query_seconds = 0.0;
    for (const DatasetBaseline& b : baselines) {
        logger.line("本次范围查询输入：查询的试验品: " + b.dataset_id +
                    "；查询时间区间:-∞到+∞；查询列:" + join(c.columns, ","));
        logger.line("查询结果如下");

        const Clock::time_point t0 = Clock::now();
        std::string err;
        const auto segs = causal_meta_query_dataset_from_file(
                paths.meta_dir.string(), b.dataset_id, "", "", c.columns, &err);
        if (!err.empty()) throw std::runtime_error(err);
        std::vector<std::string> lines;
        lines.reserve(segs.size());
        for (const auto& seg : segs) lines.push_back(format_query_segment(seg, c.columns));
        const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        total_query_seconds += sec;

        // 打印 I/O 不计入范围查询算法耗时；计时严格覆盖读 C、筛选/投影、结果字符串排序与组织。
        for (const std::string& x : lines) logger.line(x);
        std::ostringstream oss;
        oss << "本次范围查询小节：耗时:" << std::fixed << std::setprecision(6) << sec << 's';
        logger.line(oss.str());
        logger.line();
    }
    return total_query_seconds;
}

static std::vector<DatasetBaseline> register_datasets(const DiscoveredData& d,
                                                      const ExperimentCase& c,
                                                      const std::vector<std::string>& site_ids) {
    std::vector<DatasetBaseline> baselines;
    baselines.reserve(site_ids.size());
    for (const std::string& site_id : site_ids) {
        DatasetBaseline b = load_baseline(d, site_id, c);
        CausalMetaDatasetConfig cfg;
        cfg.dataset_id = b.dataset_id;
        cfg.display_name = site_id;
        cfg.variable_names = c.columns;
        cfg.ordered_timestamps = b.timestamps;
        cfg.sampling_step = 1;
        cfg.window_length = c.window_length;
        cfg.tau = c.tau;
        cfg.alpha = 0.01;
        cfg.infer_self = false;  // 本实验明确忽略自因果。
        cfg.use_covariance_cache_cdit = c.use_covmeta;
        std::string err;
        if (!causal_meta_register_dataset(cfg, &err)) throw std::runtime_error(err);
        baselines.push_back(std::move(b));
    }
    return baselines;
}

static void run_one(const ExperimentPaths& paths,
                    const DiscoveredData& discovered,
                    const ExperimentCase& c,
                    const std::vector<std::string>& site_ids,
                    const std::string& severity,
                    int experiment_index,
                    ExperimentLogger& logger) {
    // 每次实验开头都先把元数据目录清空；此清理时间不进入任何实验耗时。
    fs::remove_all(paths.meta_dir);
    fs::create_directories(paths.meta_dir);

    causal_meta_reset_all();
    causal_meta_set_enabled_cpp(true);
    causal_meta_set_metadata_dir(paths.meta_dir.string());
    causal_meta_set_page_logging(c.print_process_log);
    causal_meta_set_log_sink(&ExperimentLogger::causal_sink, &logger);

    const std::vector<DatasetBaseline> baselines = register_datasets(discovered, c, site_ids);

    rocksdb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = static_cast<std::size_t>(c.memtable_mb) * 1024ULL * 1024ULL;
    // 一个 active + 一个 immutable；每个 immutable 单独 Flush，便于“页面=Immutable MemTable”计数。
    options.max_write_buffer_number = 2;
    options.min_write_buffer_number_to_merge = 1;
    options.max_background_flushes = 1;
    options.max_background_compactions = 1;

    std::string attach_error;
    if (!causal_meta_attach_to_rocksdb_options(&options, &attach_error)) {
        throw std::runtime_error(attach_error);
    }

    // 第一次实验前/每次实验开始前：清理异常强退可能留下的 DB/WAL/SST/MANIFEST。
    destroy_db_state(paths.db_dir, options);

    logger.line("第" + std::to_string(experiment_index) + "次实验");
    logger.line("数据库参数设置：MemTable=" + std::to_string(c.memtable_mb) + "MB");
    logger.line("本次插入实验参数设置：插入时间范围=" + c.time_range_text +
                "；" + target_description(c, site_ids) +
                "；分辨率=" + resolution_label(c.resolution) +
                "；插入时参与列=" + join(c.columns, ",") +
                "；T=" + std::to_string(c.window_length) +
                "；τ=" + std::to_string(c.tau) +
                "；是否使用协方差缓存驱动CDiT：" + yesno(c.use_covmeta) +
                "；故障程度=" + (severity.empty() ? "无" : severity) + "；");
    logger.line();

    std::unique_ptr<rocksdb::DB> db;
    const rocksdb::Status os = rocksdb::DB::Open(options, paths.db_dir.string(), &db);
    if (!os.ok()) throw std::runtime_error("RocksDB Open 失败: " + os.ToString());

    const Clock::time_point insert0 = Clock::now();
    try {
        for (const DatasetBaseline& b : baselines) {
            insert_dataset(db.get(),
                           choose_data_file(discovered, b.site_id, c.resolution, severity),
                           b,
                           c);
        }

        // 把未达到容量阈值的最后一个 mutable MemTable 也强制 Flush 成页面；
        // wait + listener_wait 保证 Flush listener/collector 已经返回。
        rocksdb::FlushOptions fopt;
        fopt.wait = true;
        fopt.listener_wait = true;
        const rocksdb::Status fs_status = db->Flush(fopt);
        if (!fs_status.ok()) throw std::runtime_error("RocksDB Flush 失败: " + fs_status.ToString());

        causal_meta_wait_for_idle();
        std::string persist_err;
        if (!causal_meta_persist_all(&persist_err)) throw std::runtime_error(persist_err);
    } catch (...) {
        std::string ignored;
        causal_meta_before_db_close(&ignored);
        db.reset();
        causal_meta_clear_log_sink();
        try { destroy_db_state(paths.db_dir, options); } catch (...) {}
        throw;
    }
    const double insert_seconds = std::chrono::duration<double>(Clock::now() - insert0).count();
    const CausalMetaCounters counters = causal_meta_get_counters();

    {
        std::ostringstream oss;
        oss << "本次插入完毕 | 耗时：" << std::fixed << std::setprecision(6) << insert_seconds << "s；";
        logger.line(oss.str());
    }
    logger.line();

    const double query_seconds = run_queries(paths, baselines, c, logger);

    append_summary_row(paths.summary_csv,
                       c,
                       static_cast<int>(site_ids.size()),
                       insert_seconds,
                       query_seconds,
                       counters,
                       severity);

    // RocksDB 关闭前再次执行 barrier+持久化；前面已持久化且无新脏页时为快速 no-op。
    std::string close_err;
    if (!causal_meta_before_db_close(&close_err)) throw std::runtime_error(close_err);
    db.reset();
    causal_meta_clear_log_sink();

    // 每次实验结束后恢复为空库；这部分时间不记录到插入/推断/查询耗时。
    destroy_db_state(paths.db_dir, options);
}

}  // 匿名命名空间结束

int run_fluxnet_experiments() {
    try {
        const ExperimentPaths paths = resolve_paths();
        if (paths.data_dir.empty()) {
            throw std::runtime_error(
                    "未找到生物圈-大气通量数据目录。请 export FLUXNET_DATA_DIR=../remixdb/data/fluxnet_data");
        }

        const DiscoveredData discovered = discover_data(paths.data_dir);
        const std::vector<ExperimentCase> cases = load_cases(paths.input_csv);

        ExperimentLogger logger(paths.print_log);
        logger.initialize();
        initialize_summary_csv(paths.summary_csv);

        int experiment_index = 0;
        for (std::size_t case_index = 0; case_index < cases.size(); ++case_index) {
            const ExperimentCase& c = cases[case_index];
            const auto usable = build_usable_site_map(discovered, c);

            // 先验证 clean 至少能得到要求的试验品集合。
            if (c.target_is_count) {
                (void)choose_count_sites(discovered, c, usable, "");
            } else {
                (void)choose_specified_sites(discovered, c, usable, "");
            }

            const std::vector<std::string> severities = fault_severities_for_case(discovered, c, usable);
            if (c.test_fault && severities.size() == 1) {
                logger.line("警告：实验输入第" + std::to_string(case_index + 2) +
                            "行要求故障数据，但当前分辨率/站点集合没有可共同执行的故障程度；只执行无故障版本。");
            }

            for (const std::string& severity : severities) {
                const std::vector<std::string> site_ids = c.target_is_count
                        ? choose_count_sites(discovered, c, usable, severity)
                        : choose_specified_sites(discovered, c, usable, severity);
                ++experiment_index;
                run_one(paths, discovered, c, site_ids, severity, experiment_index, logger);
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "生物圈-大气通量实验失败: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "生物圈-大气通量实验失败: 未知异常" << std::endl;
        return 2;
    }
}
