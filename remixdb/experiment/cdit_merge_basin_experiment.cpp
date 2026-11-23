/*
 * 本文件实现 Basin 数据集上的相邻窗口图合并实验及其统计日志。
 */

#include "cdit_merge_basin_experiment.h"

#include "../cdit_module.h"
#include "../merge_cg.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <cstdlib>

namespace fs = std::filesystem;

namespace {

// 打印开关：
// 1) 默认仅打印“合并前后效果对比”，即 PRINT_COMPARE=true, 其余=false。
// 2) 若想额外打印“合并前”，把 PRINT_BEFORE 改为 true。
// 3) 若想额外打印“合并后”，把 PRINT_AFTER 改为 true。
    static constexpr bool PRINT_BEFORE = false;
    static constexpr bool PRINT_AFTER = false;
    static constexpr bool PRINT_COMPARE = true;

    static constexpr const char* kInputCsv = "experiment/cdit_merge_basin_experiment_input.csv";
    static constexpr const char* kDataDir = "data/basin_data";
    static constexpr const char* kStatCsv = "experiment_log/对河川径流的库外CDiT+Merge实验统计.csv";
    static constexpr const char* kPrintLog = "experiment_log/对河川径流的库外CDiT+Merge实验打印日志.txt";
    static constexpr const char* kStatHeader =
            "试验品个数或指定试验品集合,查询时间起止范围,采样步长,完整子随机过程长度T,推断时的τ,合并时的Δ,合并时的θ,耗时(s),压缩率,仅合并部分耗时\r\n";
    static constexpr double kAlpha = 0.01;

    struct Date {
        int y = 0;
        int m = 0;
        int d = 0;

        bool operator<(const Date& other) const {
            return std::tie(y, m, d) < std::tie(other.y, other.m, other.d);
        }

        bool operator==(const Date& other) const {
            return y == other.y && m == other.m && d == other.d;
        }
    };

    struct DateRange {
        bool has_start = false;
        bool has_end = false;
        Date start{};
        Date end{};
        std::string raw;
    };

    struct BasinRow {
        Date date{};
        double t = 0.0;
        double p = 0.0;
        double q = 0.0;
    };

    struct BasinSeries {
        std::string basin_id;
        std::vector<BasinRow> rows;
    };

    struct ExperimentConfig {
        std::string raw_selector;
        std::string raw_range;
        int sample_stride = 1;
        int window_T = 0;
        int tau = 0;
        int Delta = 0;
        double theta = 0.0;
        bool use_count = true;
        int sample_count = 0;
        std::vector<std::string> specified_ids;
    };

    struct WindowRecord {
        Date start_date{};
        Date end_date{};
        CDiTResult cdit;
        std::string formatted;
    };

    struct MergedRecord {
        Date start_date{};
        Date end_date{};
        CDiTResult cdit;
        std::string formatted;
        std::vector<int> source_indices;
    };

    struct ExperimentRunSummary {
        double elapsed_seconds = 0.0;
        double merge_elapsed_seconds = 0.0;
        int before_count = 0;
        int after_count = 0;
        double compression_ratio = 0.0;
    };

    class DualLogger {
    public:
        explicit DualLogger(const std::string& path) {
            out_.open(path, std::ios::binary | std::ios::app);
        }

        template <typename T>
        DualLogger& operator<<(const T& value) {
            std::cout << value;
            if (out_.is_open()) out_ << value;
            return *this;
        }

        DualLogger& operator<<(std::ostream& (*manip)(std::ostream&)) {
            manip(std::cout);
            if (out_.is_open()) manip(out_);
            return *this;
        }

        void flush() {
            std::cout.flush();
            if (out_.is_open()) out_.flush();
        }

    private:
        std::ofstream out_;
    };

    static std::string trim(const std::string& s) {
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;

        size_t j = s.size();
        while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;

        return s.substr(i, j - i);
    }

    static bool starts_with(const std::string& s, const std::string& prefix) {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    static std::string strip_utf8_bom(const std::string& s) {
        if (s.size() >= 3 &&
            static_cast<unsigned char>(s[0]) == 0xEF &&
            static_cast<unsigned char>(s[1]) == 0xBB &&
            static_cast<unsigned char>(s[2]) == 0xBF) {
            return s.substr(3);
        }
        return s;
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

    static std::string normalize_excel_wrapped(const std::string& s) {
        std::string x = trim(s);

        if (starts_with(x, "=\"") && x.size() >= 3 && x.back() == '"') {
            x = x.substr(2, x.size() - 3);
        }

        if (x.size() >= 2 && x.front() == '"' && x.back() == '"') {
            x = x.substr(1, x.size() - 2);
        }

        return trim(x);
    }

    static bool parse_date_any(const std::string& s, Date& out) {
        std::string x = trim(s);
        if (x.empty()) return false;

        std::replace(x.begin(), x.end(), '/', '-');

        int y = 0;
        int m = 0;
        int d = 0;
        char c1 = 0;
        char c2 = 0;

        std::istringstream iss(x);
        if (!(iss >> y >> c1 >> m >> c2 >> d)) return false;
        if (c1 != '-' || c2 != '-') return false;
        if (y <= 0 || m <= 0 || d <= 0) return false;

        out = Date{y, m, d};
        return true;
    }

    static std::string format_date(const Date& dt) {
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(4) << dt.y << '-'
            << std::setw(2) << dt.m << '-'
            << std::setw(2) << dt.d;
        return oss.str();
    }

    static bool in_range(const Date& d, const DateRange& range) {
        if (range.has_start && d < range.start) return false;
        if (range.has_end && range.end < d) return false;
        return true;
    }

    static bool parse_date_range(const std::string& raw, DateRange& out) {
        out = DateRange{};
        out.raw = raw;

        std::string x = normalize_excel_wrapped(raw);
        x = trim(x);

        if (x == "-∞到+∞") return true;

        const std::string delim = "到";
        size_t pos = x.find(delim);
        if (pos == std::string::npos) return false;

        std::string left = trim(x.substr(0, pos));
        std::string right = trim(x.substr(pos + delim.size()));

        if (left != "-∞") {
            if (!parse_date_any(left, out.start)) return false;
            out.has_start = true;
        }

        if (right != "+∞") {
            if (!parse_date_any(right, out.end)) return false;
            out.has_end = true;
        }

        return true;
    }

    static bool is_integer_string(const std::string& s) {
        std::string x = trim(s);
        if (x.empty()) return false;

        for (char c : x) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }

        return true;
    }

    static std::vector<std::string> split_by_space(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream iss(s);
        std::string tok;

        while (iss >> tok) {
            out.push_back(tok);
        }

        return out;
    }

    static std::string format_double_2(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << x;
        return oss.str();
    }

    static std::string format_double_4(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4) << x;
        return oss.str();
    }

    static std::string format_percent_2(double ratio) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << ratio * 100.0 << '%';
        return oss.str();
    }

    static std::string csv_escape_excel(const std::string& s) {
        std::string x = s;
        size_t pos = 0;

        while ((pos = x.find('"', pos)) != std::string::npos) {
            x.insert(pos, 1, '"');
            pos += 2;
        }

        return "=\"" + x + "\"";
    }

    static std::string format_cdit_named_sorted(const CDiTResult& r) {
        static const std::vector<std::string> names = {"T", "P", "Q"};
        return format_inferred_named(r, names);
    }

    static bool load_basin_series(const fs::path& csv_path,
                                  const DateRange& range,
                                  BasinSeries& out) {
        std::ifstream in(csv_path);
        if (!in.is_open()) return false;

        out = BasinSeries{};
        out.basin_id = csv_path.stem().string();

        if (out.basin_id.size() >= 4 &&
            out.basin_id.substr(out.basin_id.size() - 4) == "_pre") {
            out.basin_id = out.basin_id.substr(0, out.basin_id.size() - 4);
        }

        std::string line;
        bool first_line = true;

        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (first_line) {
                first_line = false;
                line = strip_utf8_bom(line);
                continue;
            }

            if (trim(line).empty()) continue;

            auto fields = parse_csv_line(line);
            if (fields.size() < 4) continue;

            Date dt{};
            if (!parse_date_any(fields[0], dt)) continue;
            if (!in_range(dt, range)) continue;

            BasinRow row;
            row.date = dt;
            row.t = std::strtod(trim(fields[1]).c_str(), nullptr);
            row.p = std::strtod(trim(fields[2]).c_str(), nullptr);
            row.q = std::strtod(trim(fields[3]).c_str(), nullptr);

            out.rows.push_back(row);
        }

        return true;
    }

    static std::vector<BasinRow> sample_rows(const std::vector<BasinRow>& rows,
                                             int stride) {
        std::vector<BasinRow> sampled;

        if (stride <= 0) stride = 1;

        for (size_t i = 0; i < rows.size(); i += static_cast<size_t>(stride)) {
            sampled.push_back(rows[i]);
        }

        return sampled;
    }

    static std::vector<WindowRecord> infer_windows(const BasinSeries& series,
                                                   int window_T,
                                                   int tau) {
        std::vector<WindowRecord> out;

        if (window_T <= 0 || tau <= 0) return out;
        if ((int)series.rows.size() < window_T) return out;

        for (size_t start = 0;
             start + static_cast<size_t>(window_T) <= series.rows.size();
             start += static_cast<size_t>(window_T)) {
            std::vector<std::vector<double>> data;
            data.reserve(static_cast<size_t>(window_T));

            for (int i = 0; i < window_T; ++i) {
                const auto& row = series.rows[start + static_cast<size_t>(i)];
                data.push_back({row.t, row.p, row.q});
            }

            WindowRecord rec;
            rec.start_date = series.rows[start].date;
            rec.end_date = series.rows[start + static_cast<size_t>(window_T) - 1].date;
            rec.cdit = run_cdit(data, tau, kAlpha, false, nullptr, nullptr);
            rec.formatted = format_cdit_named_sorted(rec.cdit);

            out.push_back(std::move(rec));
        }

        return out;
    }

    static MergedRecord make_merged_record_from_window(const WindowRecord& w, int index) {
        MergedRecord rec;
        rec.start_date = w.start_date;
        rec.end_date = w.end_date;
        rec.cdit = w.cdit;
        rec.formatted = w.formatted;
        rec.source_indices.push_back(index);
        return rec;
    }

    static MergedRecord make_merged_record_from_pair(const MergedRecord& left,
                                                     const MergedRecord& right,
                                                     const CDiTResult& merged_graph) {
        MergedRecord rec;
        rec.start_date = left.start_date;
        rec.end_date = right.end_date;
        rec.cdit = merged_graph;
        rec.formatted = format_cdit_named_sorted(rec.cdit);
        rec.source_indices.reserve(left.source_indices.size() + right.source_indices.size());
        rec.source_indices.insert(rec.source_indices.end(),
                                  left.source_indices.begin(),
                                  left.source_indices.end());
        rec.source_indices.insert(rec.source_indices.end(),
                                  right.source_indices.begin(),
                                  right.source_indices.end());
        return rec;
    }

    /*
     * 这里改为“可回退的相邻合并”。
     *
     * 原版本是单纯的从左到右流式合并：
     *   若 cur 与 next 不能合并，则立即把 cur 输出。
     * 这样会漏掉一种情况：
     *   A 与 B 先不能合并，
     *   但 B 与 C 合并成 BC 之后，
     *   A 又可以与 BC 继续合并。
     *
     * basin_experiment / causal_meta 的语义更接近论文里的
     * “不断将可合并的相邻因果图继续合并，直到不能再合并为止”。
     *
     * 因此这里使用一个栈来维护当前已经归约过的相邻区间：
     * 每加入一个新区间，就不断尝试与栈顶左邻区间继续合并。
     * 这样可以把 A + (B+C) 这类后续才变得可合并的情况也吃进去，
     * 合并结果会更接近 basin_experiment.* 的统计结果。
     */
    // 按时间顺序处理 Basin 窗口结果，并依据配置阈值合并相邻窗口的因果图。
    static std::vector<MergedRecord> merge_windows(const std::vector<WindowRecord>& before,
                                                   int tau,
                                                   int Delta,
                                                   double theta,
                                                   double* merge_elapsed_seconds) {
        std::vector<MergedRecord> reduced;
        if (before.empty()) return reduced;

        reduced.reserve(before.size());

        for (size_t i = 0; i < before.size(); ++i) {
            reduced.push_back(make_merged_record_from_window(before[i], static_cast<int>(i)));

            while (reduced.size() >= 2) {
                const MergedRecord& left = reduced[reduced.size() - 2];
                const MergedRecord& right = reduced[reduced.size() - 1];

                const auto merge_t0 = std::chrono::steady_clock::now();

                MergeCGResult mg = merge_adjacent_causal_graphs(
                        left.cdit, right.cdit, tau, Delta, theta, true);

                const auto merge_t1 = std::chrono::steady_clock::now();

                if (merge_elapsed_seconds != nullptr) {
                    *merge_elapsed_seconds +=
                            std::chrono::duration<double>(merge_t1 - merge_t0).count();
                }

                if (mg.merge != 1) break;

                MergedRecord merged = make_merged_record_from_pair(left, right, mg.merged);

                reduced.pop_back();
                reduced.pop_back();
                reduced.push_back(std::move(merged));
            }
        }

        return reduced;
    }

    static void print_before(DualLogger& logger,
                             const std::vector<WindowRecord>& before) {
        logger << "合并前\n";

        for (const auto& rec : before) {
            logger << format_date(rec.start_date) << "至" << format_date(rec.end_date)
                   << " | 推断：" << rec.formatted << '\n';
        }
    }

    static void print_after(DualLogger& logger,
                            const std::vector<MergedRecord>& after) {
        logger << "合并后\n";

        for (const auto& rec : after) {
            logger << format_date(rec.start_date) << "至" << format_date(rec.end_date)
                   << " | 推断：" << rec.formatted << '\n';
        }
    }

    static void print_compare(DualLogger& logger,
                              const std::vector<WindowRecord>& before,
                              const std::vector<MergedRecord>& after) {
        logger << "合并前后效果对比\n";

        for (const auto& rec : after) {
            if (rec.source_indices.empty()) continue;

            if (rec.source_indices.size() == 1) {
                const auto& src = before[rec.source_indices.front()];
                logger << format_date(src.start_date) << "至" << format_date(src.end_date)
                       << " | 推断：" << src.formatted << '\n';
                continue;
            }

            for (size_t k = 0; k < rec.source_indices.size(); ++k) {
                int idx = rec.source_indices[k];
                const auto& src = before[(size_t)idx];
                logger << (k == 0 ? "┌" : "├")
                       << format_date(src.start_date) << "至" << format_date(src.end_date)
                       << " | 推断：" << src.formatted << '\n';
            }

            logger << "└▷" << format_date(rec.start_date) << "至" << format_date(rec.end_date)
                   << " | 推断：" << rec.formatted << '\n';
        }
    }

    static bool list_basin_files(std::vector<fs::path>& files) {
        files.clear();

        std::error_code ec;
        if (!fs::exists(kDataDir, ec) || !fs::is_directory(kDataDir, ec)) return false;

        for (const auto& entry : fs::directory_iterator(kDataDir, ec)) {
            if (ec) return false;
            if (!entry.is_regular_file()) continue;

            const auto name = entry.path().filename().string();

            if (entry.path().extension() == ".csv" &&
                name.size() >= 8 &&
                name.substr(name.size() - 8) == "_pre.csv") {
                files.push_back(entry.path());
            }
        }

        std::sort(files.begin(), files.end(),
                  [](const fs::path& a, const fs::path& b) {
                      return a.filename().string() < b.filename().string();
                  });

        return true;
    }

    static std::vector<fs::path> choose_evenly_spaced(const std::vector<fs::path>& files,
                                                      int need) {
        std::vector<fs::path> out;

        if (files.empty() || need <= 0) return out;
        if (need >= (int)files.size()) return files;

        std::set<int> idxs;

        if (need == 1) {
            idxs.insert(0);
        } else {
            const int n = (int)files.size();

            for (int i = 0; i < need; ++i) {
                double pos = (double)i * (double)(n - 1) / (double)(need - 1);
                int idx = (int)std::llround(pos);
                idx = std::max(0, std::min(n - 1, idx));
                idxs.insert(idx);
            }

            for (int idx = 0; (int)idxs.size() < need && idx < n; ++idx) {
                idxs.insert(idx);
            }
        }

        for (int idx : idxs) {
            out.push_back(files[(size_t)idx]);
        }

        return out;
    }

    static std::vector<fs::path> choose_specified(const std::vector<fs::path>& files,
                                                  const std::vector<std::string>& ids) {
        std::map<std::string, fs::path> mp;

        for (const auto& p : files) {
            std::string stem = p.stem().string();

            if (stem.size() >= 4 && stem.substr(stem.size() - 4) == "_pre") {
                stem = stem.substr(0, stem.size() - 4);
            }

            mp[stem] = p;
        }

        std::vector<fs::path> out;

        for (const auto& id : ids) {
            auto it = mp.find(id);
            if (it != mp.end()) out.push_back(it->second);
        }

        return out;
    }

    static bool parse_config_line(const std::vector<std::string>& fields,
                                  ExperimentConfig& cfg) {
        if (fields.size() < 7) return false;

        cfg = ExperimentConfig{};
        cfg.raw_selector = normalize_excel_wrapped(fields[0]);
        cfg.raw_range = normalize_excel_wrapped(fields[1]);
        cfg.sample_stride = std::max(1, std::atoi(trim(fields[2]).c_str()));
        cfg.window_T = std::atoi(trim(fields[3]).c_str());
        cfg.tau = std::atoi(trim(fields[4]).c_str());
        cfg.Delta = std::atoi(trim(fields[5]).c_str());
        cfg.theta = std::strtod(trim(fields[6]).c_str(), nullptr);

        DateRange tmp;
        if (!parse_date_range(cfg.raw_range, tmp)) return false;

        if (is_integer_string(cfg.raw_selector)) {
            cfg.use_count = true;
            cfg.sample_count = std::atoi(cfg.raw_selector.c_str());
        } else {
            cfg.use_count = false;
            cfg.specified_ids = split_by_space(cfg.raw_selector);
        }

        return cfg.window_T > 0 && cfg.tau > 0 && cfg.Delta > 0;
    }

    // 读取 Basin 窗口合并实验参数，并校验每组配置的字段与取值。
    static bool load_configs(std::vector<ExperimentConfig>& cfgs) {
        cfgs.clear();

        std::ifstream in(kInputCsv);
        if (!in.is_open()) return false;

        std::string line;
        bool first_line = true;

        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (trim(line).empty()) continue;

            if (first_line) {
                first_line = false;
                line = strip_utf8_bom(line);
                continue;
            }

            auto fields = parse_csv_line(line);

            ExperimentConfig cfg;
            if (parse_config_line(fields, cfg)) {
                cfgs.push_back(std::move(cfg));
            }
        }

        return true;
    }

    static bool ensure_parent_dir(const std::string& path) {
        std::error_code ec;
        fs::path p(path);

        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path(), ec);
        }

        return !ec;
    }

    static void reset_output_files() {
        ensure_parent_dir(kStatCsv);
        ensure_parent_dir(kPrintLog);

        {
            std::ofstream out(kStatCsv, std::ios::binary | std::ios::trunc);
            const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            out.write(reinterpret_cast<const char*>(bom), 3);
            out << kStatHeader;
        }

        {
            std::ofstream out(kPrintLog, std::ios::binary | std::ios::trunc);
            const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            out.write(reinterpret_cast<const char*>(bom), 3);
        }
    }

    static void append_stat_csv_row(const ExperimentConfig& cfg,
                                    const ExperimentRunSummary& summary) {
        std::ofstream out(kStatCsv, std::ios::binary | std::ios::app);
        if (!out.is_open()) return;

        std::string selector_cell;
        if (cfg.use_count) {
            selector_cell = std::to_string(cfg.sample_count);
        } else {
            selector_cell = csv_escape_excel(cfg.raw_selector);
        }

        out << selector_cell << ','
            << csv_escape_excel(cfg.raw_range) << ','
            << cfg.sample_stride << ','
            << cfg.window_T << ','
            << cfg.tau << ','
            << cfg.Delta << ','
            << cfg.theta << ','
            << format_double_2(summary.elapsed_seconds) << ','
            << format_percent_2(summary.compression_ratio) << ','
            << format_double_4(summary.merge_elapsed_seconds) << "\r\n";
    }

    static DateRange get_range_checked(const std::string& raw) {
        DateRange r;
        parse_date_range(raw, r);
        return r;
    }

    // 执行单组 Basin 窗口合并实验，记录合并前后窗口数、图结构和统计信息。
    static ExperimentRunSummary run_one_experiment(int exp_idx,
                                                   const ExperimentConfig& cfg,
                                                   DualLogger& logger) {
        const auto t0 = std::chrono::steady_clock::now();

        ExperimentRunSummary summary;
        DateRange range = get_range_checked(cfg.raw_range);

        std::vector<fs::path> all_files;
        if (!list_basin_files(all_files)) {
            logger << "河川径流第" << exp_idx << "次实验\n";
            logger << "数据目录不存在或不可读取：" << kDataDir << "\n\n";
            logger.flush();
            return summary;
        }

        std::vector<fs::path> selected_files =
                cfg.use_count ? choose_evenly_spaced(all_files, cfg.sample_count)
                              : choose_specified(all_files, cfg.specified_ids);

        const int displayed_count = cfg.use_count ? cfg.sample_count
                                                  : (int)selected_files.size();

        logger << "河川径流第" << exp_idx << "次实验\n";
        logger << "本次数据库外纯CDiT推断+Merge实验参数设置：试验品个数=" << displayed_count
               << "；因果查询起止=" << cfg.raw_range
               << "；采样步长=" << cfg.sample_stride
               << "；T=" << cfg.window_T
               << "；τ=" << cfg.tau
               << "；Δ=" << cfg.Delta
               << "；θ=" << cfg.theta << "\n";

        for (const auto& path : selected_files) {
            BasinSeries series;
            if (!load_basin_series(path, range, series)) continue;

            auto sampled = sample_rows(series.rows, cfg.sample_stride);
            series.rows.swap(sampled);

            const auto before = infer_windows(series, cfg.window_T, cfg.tau);

            double one_series_merge_seconds = 0.0;
            const auto after = merge_windows(
                    before,
                    cfg.tau,
                    cfg.Delta,
                    cfg.theta,
                    &one_series_merge_seconds);

            summary.merge_elapsed_seconds += one_series_merge_seconds;
            summary.before_count += (int)before.size();
            summary.after_count += (int)after.size();

            logger << "试验品：" << series.basin_id << "\n";

            if (PRINT_BEFORE) print_before(logger, before);
            if (PRINT_AFTER) print_after(logger, after);
            if (PRINT_COMPARE) print_compare(logger, before, after);
        }

        const auto t1 = std::chrono::steady_clock::now();
        summary.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

        if (summary.before_count > 0) {
            summary.compression_ratio =
                    1.0 - (double)summary.after_count / (double)summary.before_count;
        }

        logger << "小节：耗时:" << format_double_2(summary.elapsed_seconds)
               << "s，合并前=" << summary.before_count
               << "，合并后=" << summary.after_count
               << "，压缩率=" << format_percent_2(summary.compression_ratio)
               << "，仅合并部分耗时=" << format_double_4(summary.merge_elapsed_seconds)
               << "s\n\n";
        logger.flush();

        return summary;
    }

} // 匿名命名空间结束

// Basin 窗口合并实验入口：加载配置并逐组执行合并实验。
extern "C" int run_cdit_merge_basin_experiments(void) {
    reset_output_files();

    DualLogger logger(kPrintLog);

    std::vector<ExperimentConfig> cfgs;
    if (!load_configs(cfgs)) {
        logger << "无法读取实验参数文件：" << kInputCsv << '\n';
        logger.flush();
        return 1;
    }

    for (size_t i = 0; i < cfgs.size(); ++i) {
        ExperimentRunSummary summary =
                run_one_experiment((int)i + 1, cfgs[i], logger);

        append_stat_csv_row(cfgs[i], summary);
    }

    return 0;
}
