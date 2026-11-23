/*
 * 本文件实现 Basin 数据集的实验流程，包括配置读取、数据准备、数据库写入、查询、计时和结果日志输出。
 */

#include "basin_experiment.h"

#include "../causal_meta.h"
#include "../xdb.h"

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
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

    const std::string kDataDir = "data/basin_data";
    const std::string kMetaDir = "file/basin_experiment";
    const std::string kLogDir = "experiment_log";
    const std::string kInputCsv = "experiment/basin_experiment_input.csv";
    const std::string kDbDir = "xdb_basin_experiment";
    const std::vector<std::string> kVarNames = {"T", "P", "Q"};

    struct BasinExperimentConfig {
        std::string sample_spec;
        std::string time_range_raw;
        int sampling_step = 1;
        int window_length = 365;
        int tau = 5;
        int delta = 2;
        double theta = 0.8;
        int memtable_mb = 2;
        bool test_fault = false;
        bool print_process_log = false;
        bool use_cov_cache_cdit = false;
    };

    struct TimeRange {
        bool start_inf = true;
        bool end_inf = true;
        int start_day = 0;
        int end_day = 0;
        std::string display;
    };

    struct CsvRow {
        std::vector<std::string> fields;
    };

    struct BaseTimestampInfo {
        std::string key;
        std::string label;
        int day = 0;
    };

    struct TeeLog {
        std::ofstream file;
        bool console = true;

        void write(const std::string& s) {
            if (console) {
                std::cout << s;
                std::cout.flush();
            }
            if (file.good()) {
                file << s;
                file.flush();
            }
        }
    };

    static TeeLog* g_log = nullptr;

    static void causal_log_sink(const char* s, void* userdata) {
        (void)userdata;
        if (g_log && s) {
            g_log->write(std::string(s));
        }
    }

    static double sec_since(const Clock::time_point& t0) {
        return std::chrono::duration<double>(Clock::now() - t0).count();
    }

    static std::string trim(const std::string& s) {
        size_t l = 0;
        size_t r = s.size();
        while (l < r && std::isspace((unsigned char)s[l])) {
            ++l;
        }
        while (r > l && std::isspace((unsigned char)s[r - 1])) {
            --r;
        }
        return s.substr(l, r - l);
    }

    static std::vector<std::string> split_ws_or_comma(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (std::isspace((unsigned char)c) || c == ',' || c == ';') {
                if (!cur.empty()) {
                    out.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) {
            out.push_back(cur);
        }
        return out;
    }

    static bool parse_int_strict(const std::string& s, int& v) {
        std::string x = trim(s);
        if (x.empty()) {
            return false;
        }
        for (char c : x) {
            if (!std::isdigit((unsigned char)c)) {
                return false;
            }
        }
        v = std::atoi(x.c_str());
        return true;
    }

    static bool parse_bool_cn(const std::string& s) {
        std::string x = trim(s);
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
        });
        return x == "是" || x == "true" || x == "1" || x == "yes" || x == "y";
    }

    static std::vector<std::string> parse_csv_line(const std::string& line) {
        std::vector<std::string> out;
        std::string cur;
        bool inq = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (inq) {
                if (c == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        cur.push_back('"');
                        ++i;
                    } else {
                        inq = false;
                    }
                } else {
                    cur.push_back(c);
                }
            } else {
                if (c == '"') {
                    inq = true;
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

    static std::string clean_excel_text(std::string s) {
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
            if (!s.empty() && s[0] == '=') {
                s = s.substr(1);
                changed = true;
            }
        }
        s = trim(s);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            s = s.substr(1, s.size() - 2);
        }
        return trim(s);
    }

    static int days_from_civil(int y, unsigned m, unsigned d) {
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + (int)doe - 719468;
    }

    static bool parse_date_ymd(const std::string& s, int& y, int& m, int& d) {
        std::string x = trim(s);
        char sep = x.find('/') != std::string::npos ? '/' : '-';
        std::vector<std::string> parts;
        std::string cur;
        for (char c : x) {
            if (c == sep) {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        parts.push_back(cur);
        if (parts.size() != 3) {
            return false;
        }
        y = std::atoi(parts[0].c_str());
        m = std::atoi(parts[1].c_str());
        d = std::atoi(parts[2].c_str());
        return y > 0 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
    }

    static std::string normalize_date_key(const std::string& s) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_ymd(s, y, m, d)) {
            return trim(s);
        }
        std::ostringstream oss;
        oss << std::setw(4) << std::setfill('0') << y << '-'
            << std::setw(2) << std::setfill('0') << m << '-'
            << std::setw(2) << std::setfill('0') << d;
        return oss.str();
    }

    static std::string display_date_label(const std::string& s) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_ymd(s, y, m, d)) {
            return trim(s);
        }
        std::ostringstream oss;
        oss << y << '/' << m << '/' << d;
        return oss.str();
    }

    static int date_to_day(const std::string& s) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_ymd(s, y, m, d)) {
            return 0;
        }
        return days_from_civil(y, (unsigned)m, (unsigned)d);
    }

    static TimeRange parse_time_range(const std::string& raw) {
        TimeRange tr;
        std::string x = clean_excel_text(raw);
        tr.display = x;

        if (x.empty() || x == "-∞到+∞" || x == "-inf到+inf" || x == "-INF到+INF") {
            tr.start_inf = true;
            tr.end_inf = true;
            tr.display = "-∞到+∞";
            return tr;
        }

        size_t pos = x.find("到");
        if (pos == std::string::npos) {
            pos = x.find('~');
        }
        if (pos == std::string::npos) {
            pos = x.find(',');
        }

        std::string a = pos == std::string::npos ? x : x.substr(0, pos);
        std::string b = pos == std::string::npos ? x : x.substr(pos + ((x.substr(pos, 3) == "到") ? 3 : 1));
        a = trim(a);
        b = trim(b);

        if (a != "-∞" && a != "-inf" && a != "-INF") {
            tr.start_inf = false;
            tr.start_day = date_to_day(a);
        }
        if (b != "+∞" && b != "+inf" && b != "+INF") {
            tr.end_inf = false;
            tr.end_day = date_to_day(b);
        }
        if (tr.display.empty()) {
            tr.display = x;
        }
        return tr;
    }

    static bool in_range_day(int day, const TimeRange& tr) {
        if (!tr.start_inf && day < tr.start_day) {
            return false;
        }
        if (!tr.end_inf && day > tr.end_day) {
            return false;
        }
        return true;
    }

    static std::string csv_escape(const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') {
                out += "\"\"";
            } else {
                out.push_back(c);
            }
        }
        out += "\"";
        return out;
    }

    static std::string excel_text_cell(const std::string& s) {
        return csv_escape("=\"" + s + "\"");
    }

    static std::string fixed(double v, int digits = 4) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(digits) << v;
        return oss.str();
    }

    static std::vector<BasinExperimentConfig> default_configs() {
        BasinExperimentConfig cfg;
        cfg.sample_spec = "2";
        cfg.time_range_raw = "-∞到+∞";
        cfg.sampling_step = 1;
        cfg.window_length = 365;
        cfg.tau = 5;
        cfg.delta = 2;
        cfg.theta = 0.8;
        cfg.memtable_mb = 2;
        cfg.test_fault = true;
        cfg.print_process_log = true;
        cfg.use_cov_cache_cdit = true;
        return {cfg};
    }

    // 读取实验输入 CSV，并将每行配置转换为可执行的实验参数；文件缺失或没有有效配置时使用内置默认配置。
    static std::vector<BasinExperimentConfig> read_input_configs() {
        std::vector<BasinExperimentConfig> configs;
        std::ifstream in(kInputCsv);
        if (!in.good()) {
            return default_configs();
        }

        std::string line;
        bool header = true;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            if (header) {
                header = false;
                continue;
            }

            std::vector<std::string> f = parse_csv_line(line);
            if (f.size() < 10) {
                continue;
            }

            BasinExperimentConfig cfg;
            cfg.sample_spec = clean_excel_text(f[0]);
            cfg.time_range_raw = clean_excel_text(f[1]);
            cfg.sampling_step = std::max(1, std::atoi(clean_excel_text(f[2]).c_str()));
            cfg.window_length = std::max(1, std::atoi(clean_excel_text(f[3]).c_str()));
            cfg.tau = std::max(1, std::atoi(clean_excel_text(f[4]).c_str()));
            cfg.delta = std::max(1, std::atoi(clean_excel_text(f[5]).c_str()));
            cfg.theta = std::strtod(clean_excel_text(f[6]).c_str(), nullptr);
            cfg.memtable_mb = std::max(1, std::atoi(clean_excel_text(f[7]).c_str()));
            cfg.test_fault = parse_bool_cn(clean_excel_text(f[8]));
            cfg.print_process_log = parse_bool_cn(clean_excel_text(f[9]));

            if (f.size() >= 11) {
                cfg.use_cov_cache_cdit = parse_bool_cn(clean_excel_text(f[10]));
            } else {
                cfg.use_cov_cache_cdit = false;
            }

            configs.push_back(cfg);
        }

        // 只有表头而没有实验行时，表示该数据集暂不运行；不要自动启用内置默认参数。
        return configs;
    }

    // 扫描 Basin 数据目录，收集可作为基准输入的试验品编号。
    static std::vector<std::string> discover_base_ids() {
        std::vector<std::string> ids;
        if (!fs::exists(kDataDir)) {
            return ids;
        }

        for (const auto& ent : fs::directory_iterator(kDataDir)) {
            if (!ent.is_regular_file()) {
                continue;
            }
            std::string fn = ent.path().filename().string();
            const std::string suffix = "_pre.csv";
            if (fn.size() > suffix.size() && fn.substr(fn.size() - suffix.size()) == suffix) {
                ids.push_back(fn.substr(0, fn.size() - suffix.size()));
            }
        }

        std::sort(ids.begin(), ids.end());
        return ids;
    }

    // 根据配置中的数量或显式编号选择本轮参与实验的 Basin 试验品。
    static std::vector<std::string> select_dataset_ids(const std::string& sample_spec, const std::vector<std::string>& all_ids) {
        int n = 0;
        if (parse_int_strict(sample_spec, n)) {
            if (n <= 0 || all_ids.empty()) {
                return {};
            }
            if (n >= (int)all_ids.size()) {
                return all_ids;
            }

            std::vector<std::string> ids;
            if (n == 1) {
                ids.push_back(all_ids.front());
                return ids;
            }

            std::set<int> used;
            for (int i = 0; i < n; ++i) {
                int idx = (int)std::llround((double)i * (double)(all_ids.size() - 1) / (double)(n - 1));
                idx = std::max(0, std::min(idx, (int)all_ids.size() - 1));
                used.insert(idx);
            }

            for (int idx : used) {
                ids.push_back(all_ids[(size_t)idx]);
            }
            return ids;
        }

        std::vector<std::string> ids = split_ws_or_comma(sample_spec);
        std::vector<std::string> out;
        std::set<std::string> all(all_ids.begin(), all_ids.end());
        for (const std::string& id : ids) {
            if (all.count(id)) {
                out.push_back(id);
            }
        }
        return out;
    }

    static std::vector<std::string> discover_fault_suffixes(const std::vector<std::string>& ids) {
        static const std::vector<std::string> desired = {
                "3%_[3,182]_[7,365]",
                "5%_[3,182]_[7,365]",
                "10%_[3,182]_[7,365]",
                "15%_[3,182]_[7,365]",
                "30%_[3,182]_[7,365]",
                "50%_[3,182]_[7,365]"
        };

        std::vector<std::string> out;
        for (const std::string& suffix : desired) {
            bool ok = true;
            for (const std::string& id : ids) {
                fs::path p = fs::path(kDataDir) / (id + "_pre_" + suffix + ".csv");
                if (!fs::exists(p)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                out.push_back(suffix);
            }
        }
        return out;
    }

    // 读取基准数据的时间戳，按查询时间范围过滤并去除重复时间点。
    static std::vector<BaseTimestampInfo> read_base_timestamps(const std::string& id, const TimeRange& tr) {
        std::vector<BaseTimestampInfo> ts;
        fs::path path = fs::path(kDataDir) / (id + "_pre.csv");
        std::ifstream in(path);
        if (!in.good()) {
            return ts;
        }

        std::string line;
        bool header = true;
        std::set<std::string> seen;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            if (header) {
                header = false;
                continue;
            }

            std::vector<std::string> f = parse_csv_line(line);
            if (f.empty()) {
                continue;
            }

            int day = date_to_day(f[0]);
            if (!in_range_day(day, tr)) {
                continue;
            }

            std::string key = normalize_date_key(f[0]);
            if (!seen.insert(key).second) {
                continue;
            }

            BaseTimestampInfo info;
            info.key = key;
            info.label = display_date_label(f[0]);
            info.day = day;
            ts.push_back(std::move(info));
        }

        return ts;
    }

    // 依据基准时间戳和窗口参数注册数据集，使后续页面回调能够维护对应的因果元数据。
    static bool register_dataset_for_meta(const std::string& id, const BasinExperimentConfig& cfg, const TimeRange& tr) {
        std::vector<BaseTimestampInfo> base = read_base_timestamps(id, tr);
        if (base.empty()) {
            return false;
        }

        CausalMetaDatasetConfig mc;
        mc.dataset_id = id;
        mc.display_name = id;
        mc.variable_names = kVarNames;
        mc.sampling_step = cfg.sampling_step;
        mc.window_length = cfg.window_length;
        mc.tau = cfg.tau;
        mc.delta = cfg.delta;
        mc.theta = cfg.theta;
        mc.alpha = 0.01;
        mc.infer_self = false;
        mc.ignore_self_in_merge = true;
        mc.metadata_dir = kMetaDir;
        mc.use_covariance_cache_cdit = cfg.use_cov_cache_cdit;

        mc.ordered_timestamps.reserve(base.size());
        for (const auto& b : base) {
            mc.ordered_timestamps.push_back({b.key, b.label});
        }

        return causal_meta_register_dataset(mc);
    }

    static std::string basin_file_for_suffix(const std::string& id, const std::string& suffix) {
        if (suffix.empty()) {
            return (fs::path(kDataDir) / (id + "_pre.csv")).string();
        }
        return (fs::path(kDataDir) / (id + "_pre_" + suffix + ".csv")).string();
    }

    // 解析指定 Basin CSV，将时间戳与观测值编码为键值对并写入 RemixDB。
    static long long insert_basin_file(struct xdb_ref* ref, const std::string& id, const std::string& path, const TimeRange& tr) {
        std::ifstream in(path);
        if (!in.good()) {
            return 0;
        }

        std::string line;
        bool header = true;
        long long rows = 0;

        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            if (header) {
                header = false;
                continue;
            }

            std::vector<std::string> f = parse_csv_line(line);
            if (f.size() < 4) {
                continue;
            }

            int day = date_to_day(f[0]);
            if (!in_range_day(day, tr)) {
                continue;
            }

            std::string key = id + "," + normalize_date_key(f[0]);
            std::string value = trim(f[1]) + "," + trim(f[2]) + "," + trim(f[3]);
            remixdb_put(ref, key.data(), (u32)key.size(), value.data(), (u32)value.size());
            ++rows;
        }

        return rows;
    }

    static void clear_path(const std::string& p) {
        std::error_code ec;
        fs::remove_all(p, ec);
        fs::create_directories(p, ec);
    }

    static void append_summary_header(std::ofstream& out) {
        out << "\xEF\xBB\xBF";
        out << "插入的试验品的数量,"
            << "插入的时间范围,"
            << "插入时的T,"
            << "推断时的τ,"
            << "采样步长,"
            << "合并时的Δ,"
            << "合并时的θ,"
            << "MemTable(MB),"
            << "是否测试故障数据集,"
            << "是否打印过程日志,"
            << "是否使用协方差缓存驱动CDiT,"
            << "插入耗时(s),"
            << "合并压缩率,"
            << "仅合并部分耗时(s),"
            << "全数据集全跨度全列范围查询总耗时(s),"
            << "库内推断部分耗时(s),"
            << "故障程度,"
            << "全部页面数,"
            << "互补页面数,"
            << "重叠页面数,"
            << "插入总耗时减库内推断耗时(s)\n";
    }

    static void write_summary_row(std::ofstream& out,
                                  const BasinExperimentConfig& cfg,
                                  int selected_count,
                                  const std::string& time_range_display,
                                  const std::string& fault_degree,
                                  double insert_seconds,
                                  double query_seconds,
                                  const CausalMetaCounters& counters) {
        double mcr = 0.0;
        if (counters.original_complete_windows > 0) {
            mcr = 100.0 * (double)(counters.original_complete_windows - counters.final_metadata_entries)
                  / (double)counters.original_complete_windows;
            if (mcr < 0.0) {
                mcr = 0.0;
            }
        }

        const double db_seconds = insert_seconds - counters.metadata_seconds;

        out << selected_count << ','
            << excel_text_cell(time_range_display) << ','
            << cfg.window_length << ','
            << cfg.tau << ','
            << cfg.sampling_step << ','
            << cfg.delta << ','
            << cfg.theta << ','
            << cfg.memtable_mb << ','
            << (cfg.test_fault ? "是" : "否") << ','
            << (cfg.print_process_log ? "是" : "否") << ','
            << (cfg.use_cov_cache_cdit ? "是" : "否") << ','
            << fixed(insert_seconds, 4) << ','
            << csv_escape(fixed(mcr, 2) + "%") << ','
            << fixed(counters.merge_seconds, 4) << ','
            << fixed(query_seconds, 4) << ','
            << fixed(counters.metadata_seconds, 4) << ','
            << csv_escape(fault_degree.empty() ? "无" : fault_degree) << ','
            << counters.total_pages << ','
            << counters.complementary_pages << ','
            << counters.overlapped_pages << ','
            << fixed(db_seconds, 4) << "\n";

        out.flush();
    }

    static double run_queries_and_print(const std::vector<std::string>& ids, TeeLog& log) {
        double total_query_seconds = 0.0;

        for (const std::string& id : ids) {
            auto q0 = Clock::now();
            std::vector<CausalMetaQuerySegment> result =
                    causal_meta_query_dataset_from_file(kMetaDir, id, "-∞", "+∞", kVarNames);
            double one_query_seconds = sec_since(q0);
            total_query_seconds += one_query_seconds;

            std::ostringstream oss;
            oss << "本次范围查询输入：查询的试验品: " << id
                << "；查询时间区间:-∞到+∞；查询列:T,P,Q\n";
            oss << "查询结果如下\n";
            for (const auto& seg : result) {
                oss << causal_meta_format_query_segment(seg, kVarNames) << "\n";
            }
            oss << "本次范围查询小节：耗时:" << fixed(one_query_seconds, 4) << "s\n\n";
            log.write(oss.str());
        }

        return total_query_seconds;
    }

    // 执行一组 Basin 实验：准备数据库、写入数据、刷新元数据、执行范围查询并记录分项耗时。
    static int run_one_experiment(int suite_idx,
                                  int run_idx,
                                  const BasinExperimentConfig& cfg,
                                  const std::vector<std::string>& ids,
                                  const std::string& fault_suffix,
                                  std::ofstream& summary,
                                  TeeLog& log) {
        TimeRange tr = parse_time_range(cfg.time_range_raw);

        clear_path(kMetaDir);
        clear_path(kDbDir);

        causal_meta_reset_all();
        causal_meta_set_enabled(1);
        causal_meta_set_metadata_dir(kMetaDir);
        causal_meta_set_page_logging(cfg.print_process_log);
        causal_meta_set_log_sink(causal_log_sink, nullptr);

        std::vector<std::string> registered_ids;
        for (const std::string& id : ids) {
            if (register_dataset_for_meta(id, cfg, tr)) {
                registered_ids.push_back(id);
            }
        }

        std::ostringstream intro;
        intro << "第" << suite_idx << "套-第" << run_idx << "次实验\n";
        intro << "数据库参数设置：MemTable=" << cfg.memtable_mb << "MB\n";
        intro << "本此插入实验参数设置：插入时间范围=" << tr.display
              << "；T=" << cfg.window_length
              << "；τ=" << cfg.tau
              << "；Δ=" << cfg.delta
              << "；θ=" << cfg.theta
              << "；采样步长=" << cfg.sampling_step
              << "；要插入的试验品个数=" << registered_ids.size()
              << "；是否使用协方差缓存驱动CDiT：" << (cfg.use_cov_cache_cdit ? "是" : "否")
              << "；";
        if (!fault_suffix.empty()) {
            intro << "故障程度=" << fault_suffix << "；";
        }
        intro << "\n\n";
        log.write(intro.str());

        struct xdb* db = remixdb_open(kDbDir.c_str(), 256, (size_t)cfg.memtable_mb, true);
        if (!db) {
            log.write("数据库启动失败。\n");
            causal_meta_clear_log_sink();
            return 1;
        }

        struct xdb_ref* ref = remixdb_ref(db);

        auto insert_t0 = Clock::now();

        for (const std::string& id : registered_ids) {
            const std::string path = basin_file_for_suffix(id, fault_suffix);
            insert_basin_file(ref, id, path, tr);
        }

        remixdb_sync(ref);
        remixdb_unref(ref);
        remixdb_close(db);

        double insert_seconds = sec_since(insert_t0);

        CausalMetaCounters counters = causal_meta_get_counters();

        double mcr = 0.0;
        if (counters.original_complete_windows > 0) {
            mcr = 100.0 * (double)(counters.original_complete_windows - counters.final_metadata_entries)
                  / (double)counters.original_complete_windows;
            if (mcr < 0.0) {
                mcr = 0.0;
            }
        }

        {
            std::ostringstream oss;
            oss << "本此插入完毕 | 耗时：" << fixed(insert_seconds, 2)
                << "s；合并压缩率：" << fixed(mcr, 2)
                << "%；仅合并部分耗时：" << fixed(counters.merge_seconds, 4)
                << "s\n\n";
            log.write(oss.str());
        }

        double query_seconds = run_queries_and_print(registered_ids, log);

        write_summary_row(summary,
                          cfg,
                          (int)registered_ids.size(),
                          tr.display,
                          fault_suffix.empty() ? "无" : fault_suffix,
                          insert_seconds,
                          query_seconds,
                          counters);

        causal_meta_clear_log_sink();

        clear_path(kDbDir);
        return 0;
    }

} // 匿名命名空间结束

// Basin 实验总入口：读取配置，枚举基准数据和故障版本，并逐项调用单次实验流程。
extern "C" int run_basin_experiments(void) {
    fs::create_directories(kLogDir);
    fs::create_directories(kMetaDir);
    fs::create_directories(kDataDir);

    TeeLog log;
    log.file.open(fs::path(kLogDir) / "河川径流实验全部打印日志.txt",
                  std::ios::binary | std::ios::trunc);
    g_log = &log;

    std::ofstream summary(fs::path(kLogDir) / "河川径流实验全部小节.csv",
                          std::ios::binary | std::ios::trunc);
    append_summary_header(summary);

    std::vector<BasinExperimentConfig> configs = read_input_configs();
    std::vector<std::string> all_ids = discover_base_ids();

    if (configs.empty()) {
        log.write("Basin 实验输入配置为空，按数据集公开策略跳过该实验。\n");
        g_log = nullptr;
        return 0;
    }
    if (all_ids.empty()) {
        log.write("未在 data/basin_data 中发现 *_pre.csv 数据文件，跳过 Basin 实验。\n");
        g_log = nullptr;
        return 0;
    }

    int suite_idx = 0;
    for (const BasinExperimentConfig& cfg : configs) {
        ++suite_idx;

        std::vector<std::string> ids = select_dataset_ids(cfg.sample_spec, all_ids);
        if (ids.empty()) {
            log.write("第" + std::to_string(suite_idx) + "套实验没有可用试验品，跳过。\n");
            continue;
        }

        std::vector<std::string> suffixes;
        suffixes.push_back("");

        if (cfg.test_fault) {
            std::vector<std::string> fsuf = discover_fault_suffixes(ids);
            suffixes.insert(suffixes.end(), fsuf.begin(), fsuf.end());
        }

        int run_idx = 0;
        for (const std::string& suffix : suffixes) {
            ++run_idx;
            run_one_experiment(suite_idx, run_idx, cfg, ids, suffix, summary, log);
        }
    }

    g_log = nullptr;
    return 0;
}
