/*
 * 本文件实现 Fluxnet 数据集上的相邻窗口图合并实验及其统计日志。
 */

#include "cdit_merge_fluxnet_experiment.h"

#include "../cdit_module.h"
#include "../merge_cg.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

// 打印开关：
// 1) 只打印“合并前后效果对比”（默认）：保持下面 3 个常量为 false / false / true。
// 2) 若想额外打印“合并前”，把 PRINT_PRE_MERGE 改为 true。
// 3) 若想额外打印“合并后”，把 PRINT_POST_MERGE 改为 true。
    static const bool PRINT_PRE_MERGE = false;
    static const bool PRINT_POST_MERGE = false;
    static const bool PRINT_COMPARE = true;

    static const char* const INPUT_CSV_PATH = "experiment/cdit_merge_fluxnet_experiment_input.csv";
    static const char* const DATA_ROOT = "data/fluxnet_data";
    static const char* const STAT_CSV_PATH = "experiment_log/对生物圈大气通量的库外CDiT+Merge实验统计.csv";
    static const char* const LOG_TXT_PATH = "experiment_log/对生物圈大气通量的库外CDiT+Merge实验打印日志.txt";
    static const double DEFAULT_ALPHA = 0.01;
    static const int FIXED_SAMPLE_STEP = 1;

    struct DateTime {
        int year = 0;
        int month = 1;
        int day = 1;
        int hour = 0;
        int minute = 0;
    };

    static bool operator<(const DateTime& a, const DateTime& b) {
        if (a.year != b.year) return a.year < b.year;
        if (a.month != b.month) return a.month < b.month;
        if (a.day != b.day) return a.day < b.day;
        if (a.hour != b.hour) return a.hour < b.hour;
        return a.minute < b.minute;
    }

    static bool operator==(const DateTime& a, const DateTime& b) {
        return a.year == b.year && a.month == b.month && a.day == b.day &&
               a.hour == b.hour && a.minute == b.minute;
    }

    static bool operator<=(const DateTime& a, const DateTime& b) {
        return (a < b) || (a == b);
    }

    struct TimeRange {
        bool unbounded = false;
        DateTime start;
        DateTime end;
        std::string text;
    };

    struct ExperimentConfig {
        std::string raw_sample_selector;
        bool use_explicit_sites = false;
        int requested_site_count = 0;
        std::vector<std::string> explicit_site_ids;

        TimeRange query_range;
        std::string resolution_text;
        std::string resolution_code;
        std::vector<std::string> selected_columns;
        int window_T = 0;
        int tau = 0;
        int Delta = 0;
        double theta = 0.0;
    };

    struct FluxRecord {
        DateTime ts;
        std::string ts_text;
        std::vector<double> values;
    };

    struct WindowResult {
        std::size_t row_begin = 0;
        std::size_t row_end = 0;
        DateTime ts_begin;
        DateTime ts_end;
        std::string begin_text;
        std::string end_text;
        CDiTResult result;
    };

    struct MergeGroup {
        std::vector<WindowResult> members;
        WindowResult merged;
    };

    struct SiteRunSummary {
        std::string dataset_name;
        std::vector<WindowResult> pre_merge;
        std::vector<MergeGroup> groups;
        double merge_elapsed_seconds = 0.0;
    };

    struct ExperimentSummary {
        double elapsed_seconds = 0.0;
        double merge_elapsed_seconds = 0.0;
        std::size_t total_pre_lines = 0;
        std::size_t total_post_lines = 0;
        std::string variable_frequency_text;
    };

    static std::string trim(const std::string& s) {
        const std::string spaces = " \t\r\n";
        const std::size_t left = s.find_first_not_of(spaces);
        if (left == std::string::npos) return "";
        const std::size_t right = s.find_last_not_of(spaces);
        return s.substr(left, right - left + 1);
    }

    static bool starts_with(const std::string& s, const std::string& prefix) {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    static bool ends_with(const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
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

    static std::vector<std::string> split_csv_line(const std::string& line) {
        std::vector<std::string> fields;
        std::string cur;
        bool in_quotes = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char ch = line[i];
            if (ch == '"') {
                if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    in_quotes = !in_quotes;
                }
            } else if (ch == ',' && !in_quotes) {
                fields.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        fields.push_back(cur);
        return fields;
    }

    static std::string csv_escape(const std::string& s) {
        bool need_quote = false;
        for (char ch : s) {
            if (ch == ',' || ch == '"' || ch == '\n' || ch == '\r') {
                need_quote = true;
                break;
            }
        }
        if (!need_quote) return s;
        std::string out = "\"";
        for (char ch : s) {
            if (ch == '"') out += "\"\"";
            else out.push_back(ch);
        }
        out += '"';
        return out;
    }

    static std::string csv_escape_excel_text(const std::string& s) {
        std::string inner;
        inner.reserve(s.size() + 4);
        inner += "=\"";
        for (char ch : s) {
            if (ch == '"') inner += "\"\"";
            else inner.push_back(ch);
        }
        inner += "\"";
        return csv_escape(inner);
    }

    static std::vector<std::string> split_by_char(const std::string& s, char delim) {
        std::vector<std::string> out;
        std::string cur;
        std::istringstream iss(s);
        while (std::getline(iss, cur, delim)) {
            const std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
        }
        return out;
    }

    static std::vector<std::string> split_by_space(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream iss(s);
        std::string token;
        while (iss >> token) out.push_back(trim(token));
        return out;
    }

    static std::string join_strings(const std::vector<std::string>& v, const std::string& sep) {
        std::ostringstream oss;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i != 0) oss << sep;
            oss << v[i];
        }
        return oss.str();
    }

    static std::string format_datetime(const DateTime& dt) {
        std::ostringstream oss;
        oss << std::setfill('0')
            << std::setw(4) << dt.year << '-'
            << std::setw(2) << dt.month << '-'
            << std::setw(2) << dt.day
            << ' '
            << dt.hour << ':' << dt.minute;
        return oss.str();
    }

    static bool parse_query_datetime(const std::string& text, DateTime& out) {
        std::string s = trim(text);
        if (s.empty()) return false;
        for (char& ch : s) {
            if (ch == '/' || ch == '.') ch = '-';
        }

        std::string date_part;
        std::string time_part;
        const std::size_t sp = s.find(' ');
        if (sp == std::string::npos) {
            date_part = s;
            time_part = "0:0";
        } else {
            date_part = trim(s.substr(0, sp));
            time_part = trim(s.substr(sp + 1));
            if (time_part.empty()) time_part = "0:0";
        }

        const std::vector<std::string> date_tokens = split_by_char(date_part, '-');
        if (date_tokens.empty()) return false;

        out = DateTime();
        out.year = std::atoi(date_tokens[0].c_str());
        if (date_tokens.size() >= 2) out.month = std::atoi(date_tokens[1].c_str());
        if (date_tokens.size() >= 3) out.day = std::atoi(date_tokens[2].c_str());

        const std::vector<std::string> time_tokens = split_by_char(time_part, ':');
        if (!time_tokens.empty()) out.hour = std::atoi(time_tokens[0].c_str());
        if (time_tokens.size() >= 2) out.minute = std::atoi(time_tokens[1].c_str());
        return true;
    }

    static bool parse_data_timestamp(const std::string& raw, const std::string& resolution_code, DateTime& out) {
        const std::string s = trim(raw);
        out = DateTime();
        if (resolution_code == "HH") {
            if (s.size() < 12) return false;
            out.year = std::atoi(s.substr(0, 4).c_str());
            out.month = std::atoi(s.substr(4, 2).c_str());
            out.day = std::atoi(s.substr(6, 2).c_str());
            out.hour = std::atoi(s.substr(8, 2).c_str());
            out.minute = std::atoi(s.substr(10, 2).c_str());
            return true;
        }
        if (resolution_code == "DD" || resolution_code == "WW") {
            if (s.size() < 8) return false;
            out.year = std::atoi(s.substr(0, 4).c_str());
            out.month = std::atoi(s.substr(4, 2).c_str());
            out.day = std::atoi(s.substr(6, 2).c_str());
            return true;
        }
        if (resolution_code == "MM") {
            if (s.size() < 6) return false;
            out.year = std::atoi(s.substr(0, 4).c_str());
            out.month = std::atoi(s.substr(4, 2).c_str());
            out.day = 1;
            return true;
        }
        if (resolution_code == "YY") {
            if (s.size() < 4) return false;
            out.year = std::atoi(s.substr(0, 4).c_str());
            out.month = 1;
            out.day = 1;
            return true;
        }
        return false;
    }

    static bool within_time_range(const DateTime& ts, const TimeRange& tr) {
        if (tr.unbounded) return true;
        return tr.start <= ts && ts <= tr.end;
    }

    static std::string normalize_excel_wrapped_text(std::string t) {
        t = trim(t);
        if (starts_with(t, "=\"") && t.size() >= 3 && t.back() == '"') {
            t = t.substr(2, t.size() - 3);
        }
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
            t = t.substr(1, t.size() - 2);
        }
        return trim(t);
    }

    static std::string normalize_range_text_for_print(const std::string& s) {
        std::string t = normalize_excel_wrapped_text(s);
        if (t.find("-∞") != std::string::npos && t.find("+∞") != std::string::npos) {
            return "-∞到+∞";
        }
        return t;
    }

    static TimeRange parse_time_range(const std::string& raw) {
        TimeRange tr;
        tr.text = normalize_range_text_for_print(raw);
        if (tr.text == "-∞到+∞") {
            tr.unbounded = true;
            return tr;
        }

        const std::size_t pos = tr.text.find("到");
        if (pos == std::string::npos) {
            tr.unbounded = true;
            tr.text = "-∞到+∞";
            return tr;
        }

        const std::string left = trim(tr.text.substr(0, pos));
        const std::string right = trim(tr.text.substr(pos + std::string("到").size()));
        DateTime start;
        DateTime end;
        if (!parse_query_datetime(left, start) || !parse_query_datetime(right, end)) {
            tr.unbounded = true;
            tr.text = "-∞到+∞";
            return tr;
        }
        tr.start = start;
        tr.end = end;
        return tr;
    }

    static std::string resolution_to_code(const std::string& resolution_text) {
        if (resolution_text == "按半小时") return "HH";
        if (resolution_text == "按日") return "DD";
        if (resolution_text == "按周") return "WW";
        if (resolution_text == "按月") return "MM";
        if (resolution_text == "按年") return "YY";
        return "";
    }

    static std::map<std::string, std::string> build_short_to_header() {
        std::map<std::string, std::string> mp;
        mp["R"] = "SW_IN_F_MDS";
        mp["N"] = "NEE_VUT_USTAR50";
        mp["T"] = "TA_F_MDS";
        mp["V"] = "VPD_F_MDS";
        mp["H"] = "H_F_MDS";
        mp["L"] = "LE_F_MDS";
        mp["P"] = "P_F";
        mp["G"] = "GPP_NT_VUT_USTAR50";
        return mp;
    }

    static std::string detect_site_id_from_folder(const std::string& folder_name) {
        if (!starts_with(folder_name, "FLX_")) return "";
        const std::size_t p1 = std::string("FLX_").size();
        const std::size_t p2 = folder_name.find("_FLUXNET", p1);
        if (p2 == std::string::npos || p2 <= p1) return "";
        return folder_name.substr(p1, p2 - p1);
    }

    static std::string resolution_marker(const std::string& code) {
        return std::string("_") + code + "_";
    }

    static std::string strip_csv_and_pre(const std::string& filename) {
        std::string x = filename;
        if (ends_with(x, ".csv")) x = x.substr(0, x.size() - 4);
        if (ends_with(x, "_pre")) x = x.substr(0, x.size() - 4);
        return x;
    }

    static bool find_resolution_csv(const fs::path& site_dir,
                                    const std::string& resolution_code,
                                    fs::path& out_csv_path,
                                    std::string& out_dataset_name) {
        if (!fs::exists(site_dir) || !fs::is_directory(site_dir)) return false;
        const std::string marker = resolution_marker(resolution_code);
        std::vector<fs::path> candidates;
        for (const auto& entry : fs::directory_iterator(site_dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (!ends_with(name, "_pre.csv")) continue;
            if (name.find(marker) == std::string::npos) continue;
            candidates.push_back(entry.path());
        }
        if (candidates.empty()) return false;
        std::sort(candidates.begin(), candidates.end());
        out_csv_path = candidates.front();
        out_dataset_name = strip_csv_and_pre(out_csv_path.filename().string());
        return true;
    }

    static std::vector<fs::path> list_fluxnet_site_dirs() {
        std::vector<fs::path> dirs;
        if (!fs::exists(DATA_ROOT) || !fs::is_directory(DATA_ROOT)) return dirs;
        for (const auto& entry : fs::directory_iterator(DATA_ROOT)) {
            if (!entry.is_directory()) continue;
            const std::string site_id = detect_site_id_from_folder(entry.path().filename().string());
            // ZA-Kru 属于 FLUXNET2015 Tier Two，不纳入本项目公开数据范围。
            if (site_id == "ZA-Kru") continue;
            dirs.push_back(entry.path());
        }
        std::sort(dirs.begin(), dirs.end());
        return dirs;
    }

    static std::vector<fs::path> pick_evenly_spaced(const std::vector<fs::path>& candidates, int want) {
        std::vector<fs::path> out;
        const int n = static_cast<int>(candidates.size());
        if (n == 0 || want <= 0) return out;
        if (want >= n) return candidates;
        if (want == 1) {
            out.push_back(candidates.front());
            return out;
        }

        std::set<int> used;
        for (int i = 0; i < want; ++i) {
            double pos = (double)i * (double)(n - 1) / (double)(want - 1);
            int idx = static_cast<int>(std::llround(pos));
            if (idx < 0) idx = 0;
            if (idx >= n) idx = n - 1;
            while (idx < n && used.count(idx)) ++idx;
            if (idx >= n) {
                idx = n - 1;
                while (idx >= 0 && used.count(idx)) --idx;
            }
            if (idx >= 0 && idx < n && !used.count(idx)) {
                used.insert(idx);
                out.push_back(candidates[idx]);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    static std::vector<fs::path> select_site_dirs(const ExperimentConfig& cfg) {
        const std::vector<fs::path> all_dirs = list_fluxnet_site_dirs();
        std::vector<fs::path> candidates;
        for (const auto& dir : all_dirs) {
            fs::path csv_path;
            std::string dataset_name;
            if (find_resolution_csv(dir, cfg.resolution_code, csv_path, dataset_name)) {
                candidates.push_back(dir);
            }
        }

        if (!cfg.use_explicit_sites) {
            return pick_evenly_spaced(candidates, cfg.requested_site_count);
        }

        std::vector<fs::path> out;
        std::set<std::string> want(cfg.explicit_site_ids.begin(), cfg.explicit_site_ids.end());
        for (const auto& dir : candidates) {
            const std::string folder = dir.filename().string();
            const std::string site_id = detect_site_id_from_folder(folder);
            if (want.count(site_id)) out.push_back(dir);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    static bool parse_experiment_config(const std::vector<std::string>& fields, ExperimentConfig& cfg) {
        if (fields.size() < 8) return false;
        cfg = ExperimentConfig();
        cfg.raw_sample_selector = trim(fields[0]);
        cfg.query_range = parse_time_range(fields[1]);
        cfg.resolution_text = trim(fields[2]);
        cfg.resolution_code = resolution_to_code(cfg.resolution_text);
        cfg.selected_columns = split_by_char(trim(fields[3]), ',');
        cfg.window_T = std::atoi(trim(fields[4]).c_str());
        cfg.tau = std::atoi(trim(fields[5]).c_str());
        cfg.Delta = std::atoi(trim(fields[6]).c_str());
        cfg.theta = std::atof(trim(fields[7]).c_str());

        if (cfg.resolution_code.empty() || cfg.selected_columns.empty() ||
            cfg.window_T <= 0 || cfg.tau <= 0 || cfg.Delta <= 0) {
            return false;
        }

        bool all_digits = !cfg.raw_sample_selector.empty();
        for (char ch : cfg.raw_sample_selector) {
            if (ch < '0' || ch > '9') {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            cfg.use_explicit_sites = false;
            cfg.requested_site_count = std::atoi(cfg.raw_sample_selector.c_str());
            return cfg.requested_site_count > 0;
        }

        cfg.use_explicit_sites = true;
        cfg.explicit_site_ids = split_by_space(cfg.raw_sample_selector);
        return !cfg.explicit_site_ids.empty();
    }

    static std::vector<ExperimentConfig> load_experiment_configs() {
        std::vector<ExperimentConfig> out;
        std::ifstream fin(INPUT_CSV_PATH);
        if (!fin) return out;

        std::string line;
        bool is_first_line = true;
        while (std::getline(fin, line)) {
            if (is_first_line) {
                line = strip_utf8_bom(line);
                is_first_line = false;
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (trim(line).empty()) continue;
            const std::vector<std::string> fields = split_csv_line(line);
            if (fields.empty()) continue;
            const std::string first = trim(fields[0]);
            if (first == "试验品个数或指定试验品集合") continue;
            ExperimentConfig cfg;
            if (parse_experiment_config(fields, cfg)) out.push_back(cfg);
        }
        return out;
    }

    static std::map<std::string, int> header_index_map(const std::vector<std::string>& headers) {
        std::map<std::string, int> mp;
        for (std::size_t i = 0; i < headers.size(); ++i) mp[trim(headers[i])] = static_cast<int>(i);
        return mp;
    }

    static bool load_flux_records(const fs::path& csv_path,
                                  const ExperimentConfig& cfg,
                                  std::vector<FluxRecord>& records,
                                  std::vector<std::string>& selected_short_names) {
        records.clear();
        selected_short_names.clear();

        std::ifstream fin(csv_path);
        if (!fin) return false;

        std::string line;
        if (!std::getline(fin, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> headers = split_csv_line(strip_utf8_bom(line));
        const std::map<std::string, int> header_to_idx = header_index_map(headers);
        const std::map<std::string, std::string> short_to_header = build_short_to_header();

        auto it_ts = header_to_idx.find("TIMESTAMP");
        if (it_ts == header_to_idx.end()) return false;
        const int ts_idx = it_ts->second;

        std::vector<int> value_indices;
        for (const std::string& col : cfg.selected_columns) {
            const std::string c = trim(col);
            std::string header_name = c;
            auto it_map = short_to_header.find(c);
            if (it_map != short_to_header.end()) header_name = it_map->second;
            auto it = header_to_idx.find(header_name);
            if (it == header_to_idx.end()) return false;
            value_indices.push_back(it->second);
            selected_short_names.push_back(c);
        }

        while (std::getline(fin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (trim(line).empty()) continue;
            const std::vector<std::string> fields = split_csv_line(line);
            if (fields.size() <= static_cast<std::size_t>(ts_idx)) continue;

            DateTime ts;
            if (!parse_data_timestamp(fields[ts_idx], cfg.resolution_code, ts)) continue;
            if (!within_time_range(ts, cfg.query_range)) continue;

            FluxRecord rec;
            rec.ts = ts;
            rec.ts_text = format_datetime(ts);
            rec.values.reserve(value_indices.size());

            bool ok = true;
            for (int idx : value_indices) {
                if (idx < 0 || static_cast<std::size_t>(idx) >= fields.size()) {
                    ok = false;
                    break;
                }
                try {
                    rec.values.push_back(std::stod(trim(fields[idx])));
                } catch (...) {
                    ok = false;
                    break;
                }
            }
            if (ok) records.push_back(rec);
        }
        return true;
    }

    static std::vector<WindowResult> run_pre_merge_windows(const std::vector<FluxRecord>& records,
                                                           const ExperimentConfig& cfg) {
        std::vector<WindowResult> out;
        if (records.size() < static_cast<std::size_t>(cfg.window_T)) return out;

        for (std::size_t begin = 0;
             begin + static_cast<std::size_t>(cfg.window_T) <= records.size();
             begin += static_cast<std::size_t>(cfg.window_T)) {
            const std::size_t end = begin + static_cast<std::size_t>(cfg.window_T) - 1;
            std::vector<std::vector<double>> data;
            data.reserve(static_cast<std::size_t>(cfg.window_T));
            for (std::size_t i = begin; i <= end; ++i) data.push_back(records[i].values);

            WindowResult wr;
            wr.row_begin = begin;
            wr.row_end = end;
            wr.ts_begin = records[begin].ts;
            wr.ts_end = records[end].ts;
            wr.begin_text = records[begin].ts_text;
            wr.end_text = records[end].ts_text;
            wr.result = run_cdit(data, cfg.tau, DEFAULT_ALPHA, false, nullptr, nullptr);
            out.push_back(std::move(wr));
        }
        return out;
    }

    static std::string format_result_named_in_query_order(const CDiTResult& r,
                                                          const std::vector<std::string>& short_names) {
        std::ostringstream oss;
        for (std::size_t i = 0; i < short_names.size(); ++i) {
            for (std::size_t j = 0; j < short_names.size(); ++j) {
                const auto it = r.lags_by_edge.find(std::make_pair(static_cast<int>(i + 1),
                                                                   static_cast<int>(j + 1)));
                if (it == r.lags_by_edge.end() || it->second.empty()) continue;
                std::vector<int> lags = it->second;
                std::sort(lags.begin(), lags.end());
                lags.erase(std::unique(lags.begin(), lags.end()), lags.end());
                oss << short_names[i] << "→" << short_names[j] << "滞后=";
                for (int lag : lags) oss << lag << ',';
                oss << "; ";
            }
        }
        return oss.str();
    }

    static MergeGroup make_single_group(const WindowResult& wr) {
        MergeGroup g;
        g.members.push_back(wr);
        g.merged = wr;
        return g;
    }

    static MergeGroup merge_two_groups(const MergeGroup& left,
                                       const MergeGroup& right,
                                       const CDiTResult& merged_result) {
        MergeGroup g;
        g.members.reserve(left.members.size() + right.members.size());
        g.members.insert(g.members.end(), left.members.begin(), left.members.end());
        g.members.insert(g.members.end(), right.members.begin(), right.members.end());
        g.merged = left.merged;
        g.merged.row_end = right.merged.row_end;
        g.merged.ts_end = right.merged.ts_end;
        g.merged.end_text = right.merged.end_text;
        g.merged.result = merged_result;
        return g;
    }

    /*
     * 这里不再使用原来的“单次从左到右流式合并”。
     *
     * 原实现一旦发现 current 与 next 不能合并，就立即把 current 固化输出。
     * 这会漏掉一种情况：
     *   A 与 B 先不能合并，
     *   但 B 与 C 合并成 BC 之后，
     *   A 又可以与 BC 继续合并。
     *
     * fluxnet_experiment.* / causal_meta 更接近论文中的语义：
     * “对相邻区间因果图持续合并，直到无法继续合并为止”。
     *
     * 因此这里改为“栈式可回退合并”：
     * 每加入一个新区间，就不断尝试与左邻区间继续合并；
     * 若合并成功，则继续向左回退检查；
     * 直到当前栈顶两段不能再合并为止。
     *
     * 这样能把后续才变得可合并的相邻段也并进去，使库外 CDiT+Merge 的压缩率
     * 更贴近 fluxnet_experiment.* 的结果。
     */
    // 按时间顺序从左向右合并 Fluxnet 窗口结果，并保留合并组的时间边界及因果图。
    static std::vector<MergeGroup> merge_windows_left_to_right(const std::vector<WindowResult>& pre,
                                                               const ExperimentConfig& cfg,
                                                               double* merge_elapsed_seconds) {
        std::vector<MergeGroup> reduced;
        if (pre.empty()) return reduced;

        reduced.reserve(pre.size());

        for (const WindowResult& wr : pre) {
            reduced.push_back(make_single_group(wr));

            while (reduced.size() >= 2) {
                const MergeGroup& left = reduced[reduced.size() - 2];
                const MergeGroup& right = reduced[reduced.size() - 1];

                const auto merge_t0 = std::chrono::steady_clock::now();

                const MergeCGResult mg = merge_adjacent_causal_graphs(
                        left.merged.result,
                        right.merged.result,
                        cfg.tau,
                        cfg.Delta,
                        cfg.theta,
                        true);

                const auto merge_t1 = std::chrono::steady_clock::now();

                if (merge_elapsed_seconds != nullptr) {
                    *merge_elapsed_seconds += std::chrono::duration<double>(merge_t1 - merge_t0).count();
                }

                if (mg.merge != 1) break;

                MergeGroup merged_group = merge_two_groups(left, right, mg.merged);
                reduced.pop_back();
                reduced.pop_back();
                reduced.push_back(std::move(merged_group));
            }
        }

        return reduced;
    }

    static std::string format_one_line(const std::string& begin_text,
                                       const std::string& end_text,
                                       const CDiTResult& r,
                                       const std::vector<std::string>& short_names,
                                       const std::string& prefix = "") {
        std::ostringstream oss;
        if (!prefix.empty()) oss << prefix;
        oss << begin_text << "至" << end_text << " | 推断："
            << format_result_named_in_query_order(r, short_names);
        return oss.str();
    }

    static void accumulate_variable_frequency(const std::vector<WindowResult>& pre,
                                              std::map<std::string, int>& var_counts,
                                              std::size_t& total_pre_lines,
                                              const std::vector<std::string>& short_names) {
        total_pre_lines += pre.size();
        for (const WindowResult& wr : pre) {
            std::set<int> present_indices;
            for (const auto& kv : wr.result.lags_by_edge) {
                if (kv.second.empty()) continue;
                present_indices.insert(kv.first.first);
                present_indices.insert(kv.first.second);
            }
            for (int idx : present_indices) {
                if (idx >= 1 && idx <= static_cast<int>(short_names.size())) {
                    ++var_counts[short_names[static_cast<std::size_t>(idx - 1)]];
                }
            }
        }
    }

    static std::string format_variable_frequency(const std::map<std::string, int>& var_counts,
                                                 std::size_t total_pre_lines,
                                                 const std::vector<std::string>& column_order) {
        if (total_pre_lines == 0 || var_counts.empty()) return "无";

        std::map<std::string, int> order_index;
        for (std::size_t i = 0; i < column_order.size(); ++i) order_index[column_order[i]] = static_cast<int>(i);

        struct Item {
            std::string name;
            int count;
            double ratio;
            int order;
        };

        std::vector<Item> items;
        items.reserve(var_counts.size());
        for (const auto& kv : var_counts) {
            Item it;
            it.name = kv.first;
            it.count = kv.second;
            it.ratio = 100.0 * static_cast<double>(kv.second) / static_cast<double>(total_pre_lines);
            auto found = order_index.find(kv.first);
            it.order = (found == order_index.end()) ? std::numeric_limits<int>::max() : found->second;
            items.push_back(it);
        }

        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.count != b.count) return a.count > b.count;
            if (std::fabs(a.ratio - b.ratio) > 1e-12) return a.ratio > b.ratio;
            if (a.order != b.order) return a.order < b.order;
            return a.name < b.name;
        });

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i != 0) oss << ',';
            oss << items[i].name << '(' << items[i].ratio << "%)";
        }
        return oss.str();
    }

    static std::string format_ratio_percent(std::size_t pre_count, std::size_t post_count) {
        double ratio = 0.0;
        if (pre_count > 0) {
            ratio = 100.0 * (1.0 - static_cast<double>(post_count) / static_cast<double>(pre_count));
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << ratio << '%';
        return oss.str();
    }

    static std::string format_seconds_2(double seconds) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << seconds;
        return oss.str();
    }

    static std::string format_seconds_4(double seconds) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4) << seconds;
        return oss.str();
    }

    static std::string build_site_log_text(const SiteRunSummary& site_summary,
                                           const std::vector<std::string>& selected_short_names) {
        std::ostringstream oss;
        oss << "试验品：" << site_summary.dataset_name << '\n';

        if (PRINT_PRE_MERGE) {
            oss << "合并前\n";
            for (const WindowResult& wr : site_summary.pre_merge) {
                oss << format_one_line(wr.begin_text, wr.end_text, wr.result, selected_short_names) << '\n';
            }
        }

        if (PRINT_POST_MERGE) {
            oss << "合并后\n";
            for (const MergeGroup& g : site_summary.groups) {
                oss << format_one_line(g.merged.begin_text, g.merged.end_text, g.merged.result, selected_short_names) << '\n';
            }
        }

        if (PRINT_COMPARE) {
            oss << "合并前后效果对比\n";
            for (const MergeGroup& g : site_summary.groups) {
                if (g.members.empty()) continue;

                if (g.members.size() <= 1) {
                    const WindowResult& wr = g.members.front();
                    oss << format_one_line(wr.begin_text, wr.end_text, wr.result, selected_short_names) << '\n';
                    continue;
                }

                for (std::size_t i = 0; i < g.members.size(); ++i) {
                    const std::string prefix = (i == 0) ? "┌" : "├";
                    oss << format_one_line(g.members[i].begin_text,
                                           g.members[i].end_text,
                                           g.members[i].result,
                                           selected_short_names,
                                           prefix) << '\n';
                }

                oss << format_one_line(g.merged.begin_text,
                                       g.merged.end_text,
                                       g.merged.result,
                                       selected_short_names,
                                       "└▷") << '\n';
            }
        }

        return oss.str();
    }

    static void ensure_parent_dir(const fs::path& p) {
        const fs::path parent = p.parent_path();
        if (!parent.empty()) fs::create_directories(parent);
    }

    static bool write_text_file(const fs::path& path, const std::string& content, bool append_mode) {
        ensure_parent_dir(path);
        std::ofstream fout;
        if (append_mode) fout.open(path, std::ios::out | std::ios::app | std::ios::binary);
        else fout.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!fout) return false;
        fout.write(content.data(), static_cast<std::streamsize>(content.size()));
        return true;
    }

    static bool initialize_stat_csv() {
        ensure_parent_dir(STAT_CSV_PATH);
        std::ofstream fout(STAT_CSV_PATH, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!fout) return false;
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        fout.write(reinterpret_cast<const char*>(bom), 3);
        fout << "试验品个数或指定试验品集合,查询时间起止范围,选择分辨率,参与列,完整子随机过程长度T,推断时的τ,合并时的Δ,合并时的θ,耗时(s),压缩率,变量频率排序,仅合并部分耗时\n";
        return true;
    }

    static bool append_stat_csv_row(const ExperimentConfig& cfg,
                                    const ExperimentSummary& summary) {
        std::ofstream fout(STAT_CSV_PATH, std::ios::out | std::ios::app | std::ios::binary);
        if (!fout) return false;

        const std::string selector = cfg.raw_sample_selector;
        const std::string range_field = (cfg.query_range.text == "-∞到+∞")
                                        ? csv_escape_excel_text(cfg.query_range.text)
                                        : csv_escape(cfg.query_range.text);
        const std::string columns = join_strings(cfg.selected_columns, ",");

        fout << csv_escape(selector) << ','
             << range_field << ','
             << csv_escape(cfg.resolution_text) << ','
             << csv_escape(columns) << ','
             << cfg.window_T << ','
             << cfg.tau << ','
             << cfg.Delta << ','
             << cfg.theta << ','
             << format_seconds_2(summary.elapsed_seconds) << ','
             << csv_escape(format_ratio_percent(summary.total_pre_lines, summary.total_post_lines)) << ','
             << csv_escape(summary.variable_frequency_text) << ','
             << format_seconds_4(summary.merge_elapsed_seconds) << '\n';
        return true;
    }

} // 匿名命名空间结束

// Fluxnet 窗口合并实验入口：读取配置、处理站点窗口并输出合并统计。
int run_cdit_merge_fluxnet_experiments(void) {
    const std::vector<ExperimentConfig> configs = load_experiment_configs();
    if (configs.empty()) {
        std::cerr << "未读取到 experiment/cdit_merge_fluxnet_experiment_input.csv 中的有效实验配置。" << std::endl;
        return 1;
    }

    if (!write_text_file(LOG_TXT_PATH, std::string(), false)) {
        std::cerr << "无法初始化实验打印日志文件：" << LOG_TXT_PATH << std::endl;
        return 1;
    }
    if (!initialize_stat_csv()) {
        std::cerr << "无法初始化实验统计文件：" << STAT_CSV_PATH << std::endl;
        return 1;
    }

    int experiment_index = 0;
    for (const ExperimentConfig& cfg : configs) {
        ++experiment_index;
        const auto t0 = std::chrono::steady_clock::now();

        const std::vector<fs::path> selected_sites = select_site_dirs(cfg);
        std::map<std::string, int> variable_counts;
        std::size_t total_pre_lines = 0;
        std::size_t total_post_lines = 0;
        double total_merge_elapsed_seconds = 0.0;

        std::ostringstream experiment_log;
        experiment_log << "生物圈大气通量第" << experiment_index << "次实验\n";
        experiment_log << "本次数据库外纯CDiT推断+Merge实验参数设置：试验品个数=" << selected_sites.size()
                       << "；因果查询起止=" << cfg.query_range.text
                       << "；分辨率=" << cfg.resolution_text
                       << "；参与列=" << join_strings(cfg.selected_columns, ",")
                       << "；采样步长=" << FIXED_SAMPLE_STEP
                       << "；T=" << cfg.window_T
                       << "；τ=" << cfg.tau
                       << "；Δ=" << cfg.Delta
                       << "；θ=" << cfg.theta
                       << "\n";

        for (const fs::path& site_dir : selected_sites) {
            fs::path csv_path;
            std::string dataset_name;
            if (!find_resolution_csv(site_dir, cfg.resolution_code, csv_path, dataset_name)) continue;

            std::vector<FluxRecord> records;
            std::vector<std::string> selected_short_names;
            if (!load_flux_records(csv_path, cfg, records, selected_short_names)) continue;

            SiteRunSummary site_summary;
            site_summary.dataset_name = dataset_name;
            site_summary.pre_merge = run_pre_merge_windows(records, cfg);
            site_summary.merge_elapsed_seconds = 0.0;
            site_summary.groups = merge_windows_left_to_right(
                    site_summary.pre_merge,
                    cfg,
                    &site_summary.merge_elapsed_seconds);

            total_merge_elapsed_seconds += site_summary.merge_elapsed_seconds;

            accumulate_variable_frequency(
                    site_summary.pre_merge,
                    variable_counts,
                    total_pre_lines,
                    selected_short_names);

            total_post_lines += site_summary.groups.size();
            experiment_log << build_site_log_text(site_summary, selected_short_names);
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

        ExperimentSummary summary;
        summary.elapsed_seconds = elapsed_seconds;
        summary.merge_elapsed_seconds = total_merge_elapsed_seconds;
        summary.total_pre_lines = total_pre_lines;
        summary.total_post_lines = total_post_lines;
        summary.variable_frequency_text =
                format_variable_frequency(variable_counts, total_pre_lines, cfg.selected_columns);

        experiment_log << "小结：耗时:" << format_seconds_2(summary.elapsed_seconds)
                       << "s，合并前=" << summary.total_pre_lines
                       << "，合并后=" << summary.total_post_lines
                       << "，压缩率=" << format_ratio_percent(summary.total_pre_lines, summary.total_post_lines)
                       << "，仅合并部分耗时=" << format_seconds_4(summary.merge_elapsed_seconds)
                       << "s\n";
        experiment_log << "变量频率排序：" << summary.variable_frequency_text << "\n\n";

        const std::string block = experiment_log.str();
        std::cout << block;
        write_text_file(LOG_TXT_PATH, block, true);
        append_stat_csv_row(cfg, summary);
    }

    return 0;
}
