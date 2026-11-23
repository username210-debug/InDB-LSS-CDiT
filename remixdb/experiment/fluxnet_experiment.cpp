/*
 * 本文件实现 Fluxnet 数据集的实验流程，包括站点数据读取、数据准备、数据库写入、查询、计时和结果日志输出。
 */

#include "fluxnet_experiment.h"

#include "../causal_meta.h"
#include "../xdb.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
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

namespace {

    const std::string kDataRoot = "data/fluxnet_data";
    const std::string kInputPath = "experiment/fluxnet_experiment_input.csv";
    const std::string kMetaDir = "file/fluxnet_experiment";
    const std::string kXdbDir = "xdb_fluxnet_experiment";
    const std::string kLogDir = "experiment_log";
    const std::string kSummaryPath = "experiment_log/生物圈大气通量全部实验小结.csv";
    const std::string kDetailLogPath = "experiment_log/生物圈大气通量全部实验打印日志.txt";

    struct FluxnetColumnInfo {
        std::string short_name;
        std::string csv_name;
    };

    const std::vector<FluxnetColumnInfo> kFluxnetColumns = {
            {"R", "SW_IN_F_MDS"},
            {"N", "NEE_VUT_USTAR50"},
            {"T", "TA_F_MDS"},
            {"V", "VPD_F_MDS"},
            {"H", "H_F_MDS"},
            {"L", "LE_F_MDS"},
            {"P", "P_F"},
            {"G", "GPP_NT_VUT_USTAR50"}
    };

    struct TimeRange {
        std::string raw_display;
        std::string start_key;
        std::string end_key;
        bool all_time = false;
    };

    struct ExperimentSpec {
        std::string sample_spec;
        TimeRange range;
        std::string resolution_text;
        std::string resolution_code;
        std::vector<std::string> selected_columns;
        int window_length = 30;
        int tau = 5;
        int delta = 2;
        double theta = 0.8;
        int memtable_mb = 2;
        bool test_fault = false;
        bool print_process_log = false;
        bool use_covariance_cache_cdit = false;
    };

    struct DatasetFileInfo {
        std::string folder_path;
        std::string folder_name;
        std::string site_short;
        std::string dataset_id;
        std::string query_display_name;
        std::string base_file_path;
        std::string run_file_path;
    };

    struct CsvDataRow {
        std::string timestamp_raw;
        std::string timestamp_key;
        std::string timestamp_label;
        std::vector<std::string> fields;
    };

    class ExperimentLogger {
    public:
        explicit ExperimentLogger(const std::string& path) : out_(path, std::ios::binary | std::ios::app) {}

        void write(const std::string& s) {
            std::cout << s;
            if (out_.good()) {
                out_ << s;
                out_.flush();
            }
        }

    private:
        std::ofstream out_;
    };

    struct LogSinkContext {
        ExperimentLogger* logger = nullptr;
    };

    static void meta_log_sink(const char* msg, void* userdata) {
        LogSinkContext* ctx = static_cast<LogSinkContext*>(userdata);
        if (ctx && ctx->logger && msg) ctx->logger->write(msg);
    }

    static std::string trim(const std::string& s) {
        size_t l = 0, r = s.size();
        while (l < r && std::isspace((unsigned char)s[l])) ++l;
        while (r > l && std::isspace((unsigned char)s[r - 1])) --r;
        return s.substr(l, r - l);
    }

    static bool starts_with(const std::string& s, const std::string& p) {
        return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin());
    }

    static bool ends_with(const std::string& s, const std::string& p) {
        return s.size() >= p.size() && std::equal(p.rbegin(), p.rend(), s.rbegin());
    }

    static std::vector<std::string> split_csv_line(const std::string& line) {
        std::vector<std::string> out;
        std::string cur;
        bool in_quotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (in_quotes) {
                if (c == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        cur.push_back('"');
                        ++i;
                    } else {
                        in_quotes = false;
                    }
                } else {
                    cur.push_back(c);
                }
            } else {
                if (c == '"') in_quotes = true;
                else if (c == ',') {
                    out.push_back(cur);
                    cur.clear();
                } else cur.push_back(c);
            }
        }
        out.push_back(cur);
        return out;
    }

    static std::vector<std::string> split_by_delims(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '|') {
                if (!cur.empty()) {
                    out.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    static std::string strip_excel_formula_text(std::string s) {
        s = trim(s);
        if (s.size() >= 3 && s[0] == '=' && s[1] == '"' && s.back() == '"') {
            s = s.substr(2, s.size() - 3);
        }
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            s = s.substr(1, s.size() - 2);
        }
        return trim(s);
    }

    static std::string join(const std::vector<std::string>& xs, const std::string& sep) {
        std::ostringstream oss;
        for (size_t i = 0; i < xs.size(); ++i) {
            if (i) oss << sep;
            oss << xs[i];
        }
        return oss.str();
    }

    static std::string two_digits(int x) {
        std::ostringstream oss;
        oss << std::setw(2) << std::setfill('0') << x;
        return oss.str();
    }

    static std::vector<int> extract_ints(const std::string& s) {
        std::vector<int> nums;
        std::string cur;
        for (char c : s) {
            if (std::isdigit((unsigned char)c)) cur.push_back(c);
            else if (!cur.empty()) {
                nums.push_back(std::atoi(cur.c_str()));
                cur.clear();
            }
        }
        if (!cur.empty()) nums.push_back(std::atoi(cur.c_str()));
        return nums;
    }

    static std::string canonical_from_human_time(const std::string& raw) {
        std::string s = strip_excel_formula_text(raw);
        if (s == "-∞" || s == "-inf" || s == "-INF") return "";
        if (s == "+∞" || s == "+inf" || s == "+INF") return "";
        std::vector<int> nums = extract_ints(s);
        if (nums.empty()) return "";
        int y = nums.size() > 0 ? nums[0] : 1;
        int mo = nums.size() > 1 ? nums[1] : 1;
        int d = nums.size() > 2 ? nums[2] : 1;
        int h = nums.size() > 3 ? nums[3] : 0;
        int mi = nums.size() > 4 ? nums[4] : 0;
        std::ostringstream oss;
        oss << std::setw(4) << std::setfill('0') << y
            << two_digits(mo) << two_digits(d) << two_digits(h) << two_digits(mi);
        return oss.str();
    }

    static std::string canonical_from_timestamp(const std::string& ts) {
        std::string s;
        for (char c : ts) if (std::isdigit((unsigned char)c)) s.push_back(c);
        if (s.size() >= 12) return s.substr(0, 12);
        if (s.size() == 8) return s + "0000";
        if (s.size() == 6) return s + "010000";
        if (s.size() == 4) return s + "01010000";
        if (s.size() < 12) return s + std::string(12 - s.size(), '0');
        return s;
    }

    static std::string label_from_timestamp(const std::string& ts) {
        std::string c = canonical_from_timestamp(ts);
        if (c.size() < 12) return ts;
        int y = std::atoi(c.substr(0, 4).c_str());
        int mo = std::atoi(c.substr(4, 2).c_str());
        int d = std::atoi(c.substr(6, 2).c_str());
        int h = std::atoi(c.substr(8, 2).c_str());
        int mi = std::atoi(c.substr(10, 2).c_str());
        std::ostringstream oss;
        oss << y << '-' << two_digits(mo) << '-' << two_digits(d) << ' ' << h << ':' << mi;
        return oss.str();
    }

    static TimeRange parse_time_range(const std::string& raw0) {
        TimeRange r;
        std::string raw = strip_excel_formula_text(raw0);
        r.raw_display = raw;
        size_t pos = raw.find("到");
        std::string left = pos == std::string::npos ? raw : raw.substr(0, pos);
        std::string right = pos == std::string::npos ? raw : raw.substr(pos + std::string("到").size());
        left = strip_excel_formula_text(left);
        right = strip_excel_formula_text(right);
        r.all_time = (left == "-∞" && right == "+∞") || raw == "-∞到+∞";
        if (!r.all_time) {
            if (left != "-∞") r.start_key = canonical_from_human_time(left);
            if (right != "+∞") r.end_key = canonical_from_human_time(right);
        }
        if (r.raw_display.empty()) r.raw_display = "-∞到+∞";
        return r;
    }

    static std::string resolution_to_code(const std::string& text) {
        std::string s = trim(text);
        if (s == "按半小时" || s == "半小时" || s == "HH") return "HH";
        if (s == "按日" || s == "日" || s == "DD") return "DD";
        if (s == "按周" || s == "周" || s == "WW") return "WW";
        if (s == "按月" || s == "月" || s == "MM") return "MM";
        if (s == "按年" || s == "年" || s == "YY") return "YY";
        return s;
    }

    static std::string resolution_code_to_text(const std::string& code) {
        if (code == "HH") return "按半小时";
        if (code == "DD") return "按日";
        if (code == "WW") return "按周";
        if (code == "MM") return "按月";
        if (code == "YY") return "按年";
        return code;
    }

    static bool parse_bool_cn(const std::string& s0) {
        std::string s = trim(strip_excel_formula_text(s0));
        return s == "是" || s == "yes" || s == "YES" || s == "true" || s == "TRUE" || s == "1";
    }

    static std::string fluxnet_csv_column_for_short(const std::string& short_name) {
        for (const auto& x : kFluxnetColumns) if (x.short_name == short_name) return x.csv_name;
        return short_name;
    }

    static std::string site_short_from_folder(const std::string& folder_name) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : folder_name) {
            if (c == '_') {
                parts.push_back(cur);
                cur.clear();
            } else cur.push_back(c);
        }
        parts.push_back(cur);
        if (parts.size() >= 2) return parts[1];
        if (starts_with(folder_name, "FLX_")) {
            size_t p = folder_name.find('_', 4);
            if (p != std::string::npos) return folder_name.substr(4, p - 4);
        }
        return folder_name;
    }

    static std::string remove_pre_suffix_for_display(const std::string& filename) {
        std::string name = fs::path(filename).filename().string();
        if (ends_with(name, ".csv")) name = name.substr(0, name.size() - 4);
        size_t p = name.find("_pre");
        if (p != std::string::npos) name = name.substr(0, p);
        return name;
    }

    static std::vector<std::string> list_fluxnet_folders() {
        std::vector<std::string> folders;
        if (!fs::exists(kDataRoot)) return folders;
        for (const auto& e : fs::directory_iterator(kDataRoot)) {
            if (!e.is_directory()) continue;
            const std::string folder_name = e.path().filename().string();
            // ZA-Kru 属于 FLUXNET2015 Tier Two，不纳入本项目公开数据范围。
            if (site_short_from_folder(folder_name) == "ZA-Kru") continue;
            folders.push_back(folder_name);
        }
        std::sort(folders.begin(), folders.end());
        return folders;
    }

    static std::vector<std::string> select_folders(const std::string& sample_spec_raw) {
        std::vector<std::string> all = list_fluxnet_folders();
        if (all.empty()) return all;
        std::string sample_spec = trim(sample_spec_raw);
        bool all_digits = !sample_spec.empty() && std::all_of(sample_spec.begin(), sample_spec.end(), [](char c){ return std::isdigit((unsigned char)c); });
        if (all_digits) {
            int n = std::atoi(sample_spec.c_str());
            if (n <= 0) return {};
            if (n >= (int)all.size()) return all;
            std::vector<std::string> out;
            if (n == 1) {
                out.push_back(all.front());
                return out;
            }
            std::set<int> used;
            for (int i = 0; i < n; ++i) {
                int idx = (int)std::floor((double)i * (double)(all.size() - 1) / (double)(n - 1) + 1e-9);
                if (idx < 0) idx = 0;
                if (idx >= (int)all.size()) idx = (int)all.size() - 1;
                used.insert(idx);
            }
            for (int idx : used) out.push_back(all[(size_t)idx]);
            return out;
        }

        std::set<std::string> wanted;
        for (std::string x : split_by_delims(sample_spec)) wanted.insert(trim(x));
        std::vector<std::string> out;
        for (const std::string& f : all) {
            if (wanted.count(site_short_from_folder(f)) || wanted.count(f)) out.push_back(f);
        }
        return out;
    }

    static std::string find_file_for_suffix(const std::string& folder_path, const std::string& resolution_code, const std::string& suffix) {
        if (!fs::exists(folder_path)) return std::string();
        std::vector<std::string> candidates;
        for (const auto& e : fs::directory_iterator(folder_path)) {
            if (!e.is_regular_file()) continue;
            std::string name = e.path().filename().string();
            if (name.find("_" + resolution_code + "_") == std::string::npos) continue;
            if (suffix.empty()) {
                if (ends_with(name, "_pre.csv")) candidates.push_back(e.path().string());
            } else {
                if (ends_with(name, "_pre_" + suffix + ".csv")) candidates.push_back(e.path().string());
            }
        }
        std::sort(candidates.begin(), candidates.end());
        return candidates.empty() ? std::string() : candidates.front();
    }

    static bool parse_requested_folder_count(const std::string& sample_spec_raw, int& n) {
        std::string sample_spec = trim(strip_excel_formula_text(sample_spec_raw));
        if (sample_spec.empty()) return false;
        if (!std::all_of(sample_spec.begin(), sample_spec.end(), [](unsigned char c) {
            return std::isdigit(c);
        })) {
            return false;
        }
        n = std::atoi(sample_spec.c_str());
        return true;
    }

    static bool folder_has_required_fluxnet_files(const std::string& folder_name,
                                                  const std::string& resolution_code,
                                                  const std::string& fault_suffix) {
        const std::string folder_path = kDataRoot + "/" + folder_name;
        const std::string base_file = find_file_for_suffix(folder_path, resolution_code, "");
        if (base_file.empty()) return false;

        if (fault_suffix.empty()) return true;

        const std::string run_file = find_file_for_suffix(folder_path, resolution_code, fault_suffix);
        return !run_file.empty();
    }

    static std::vector<std::string> select_folders_with_completion(const ExperimentSpec& sp,
                                                                   const std::string& fault_suffix) {
        int requested_count = 0;
        const bool count_mode = parse_requested_folder_count(sp.sample_spec, requested_count);

        if (!count_mode) {
            return select_folders(sp.sample_spec);
        }

        if (requested_count <= 0) {
            return {};
        }

        const std::vector<std::string> all_folders = list_fluxnet_folders();
        if (all_folders.empty()) {
            return {};
        }

        const std::vector<std::string> initially_selected = select_folders(sp.sample_spec);

        std::vector<std::string> completed;
        std::set<std::string> seen;

        for (const std::string& folder : initially_selected) {
            seen.insert(folder);
            if (folder_has_required_fluxnet_files(folder, sp.resolution_code, fault_suffix)) {
                completed.push_back(folder);
                if ((int)completed.size() >= requested_count) {
                    return completed;
                }
            }
        }

        for (const std::string& folder : all_folders) {
            if (seen.count(folder)) continue;
            seen.insert(folder);

            if (!folder_has_required_fluxnet_files(folder, sp.resolution_code, fault_suffix)) {
                continue;
            }

            completed.push_back(folder);
            if ((int)completed.size() >= requested_count) {
                break;
            }
        }

        return completed;
    }

    static std::vector<std::string> folders_for_fault_suffix_discovery(const ExperimentSpec& sp) {
        int requested_count = 0;
        if (parse_requested_folder_count(sp.sample_spec, requested_count)) {
            return list_fluxnet_folders();
        }
        return select_folders(sp.sample_spec);
    }

    static std::set<std::string> discover_fault_suffixes(const std::vector<std::string>& selected_folders, const std::string& resolution_code) {
        std::set<std::string> suffixes;
        for (const std::string& folder_name : selected_folders) {
            std::string folder_path = kDataRoot + "/" + folder_name;
            if (!fs::exists(folder_path)) continue;
            for (const auto& e : fs::directory_iterator(folder_path)) {
                if (!e.is_regular_file()) continue;
                std::string name = e.path().filename().string();
                if (name.find("_" + resolution_code + "_") == std::string::npos) continue;
                size_t p = name.find("_pre_");
                if (p == std::string::npos || !ends_with(name, ".csv")) continue;
                suffixes.insert(name.substr(p + 5, name.size() - (p + 5) - 4));
            }
        }
        return suffixes;
    }

    static std::map<std::string, int> header_index_map(const std::vector<std::string>& header) {
        std::map<std::string, int> m;
        for (int i = 0; i < (int)header.size(); ++i) m[trim(header[(size_t)i])] = i;
        return m;
    }

    static bool in_range(const std::string& key, const TimeRange& r) {
        if (!r.start_key.empty() && key < r.start_key) return false;
        if (!r.end_key.empty() && key > r.end_key) return false;
        return true;
    }

    static std::vector<CausalMetaTimestampInfo> load_ordered_timestamps_from_base(const std::string& base_file, const TimeRange& range) {
        std::vector<CausalMetaTimestampInfo> out;
        std::ifstream in(base_file, std::ios::binary);
        if (!in.good()) return out;
        std::string line;
        if (!std::getline(in, line)) return out;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> parts = split_csv_line(line);
            if (parts.empty()) continue;
            std::string ts = trim(parts[0]);
            std::string key = canonical_from_timestamp(ts);
            if (!in_range(key, range)) continue;
            out.push_back({ts, label_from_timestamp(ts)});
        }
        return out;
    }

    static std::vector<int> selected_column_indices_from_file(const std::string& file_path, const std::vector<std::string>& selected_columns) {
        std::ifstream in(file_path, std::ios::binary);
        std::string header_line;
        if (!std::getline(in, header_line)) return {};
        if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
        std::vector<std::string> header = split_csv_line(header_line);
        std::map<std::string, int> idx = header_index_map(header);
        std::vector<int> cols;
        for (const std::string& sc : selected_columns) {
            std::string csv_col = fluxnet_csv_column_for_short(sc);
            auto it = idx.find(csv_col);
            if (it == idx.end()) return {};
            cols.push_back(it->second);
        }
        return cols;
    }

    static std::vector<CsvDataRow> read_rows_for_insert(const std::string& file_path,
                                                        const TimeRange& range,
                                                        const std::vector<int>& selected_indices) {
        std::vector<CsvDataRow> rows;
        std::ifstream in(file_path, std::ios::binary);
        if (!in.good()) return rows;
        std::string line;
        if (!std::getline(in, line)) return rows;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> parts = split_csv_line(line);
            if (parts.empty()) continue;
            std::string ts = trim(parts[0]);
            std::string key = canonical_from_timestamp(ts);
            if (!in_range(key, range)) continue;
            CsvDataRow row;
            row.timestamp_raw = ts;
            row.timestamp_key = key;
            row.timestamp_label = label_from_timestamp(ts);
            for (int idx : selected_indices) {
                if (idx >= 0 && idx < (int)parts.size()) row.fields.push_back(trim(parts[(size_t)idx]));
                else row.fields.push_back("0");
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    static std::string value_string_from_fields(const std::vector<std::string>& fields) {
        std::ostringstream oss;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i) oss << ',';
            oss << fields[i];
        }
        return oss.str();
    }

    static std::string make_csv_field(const std::string& s0, bool excel_protect = true) {
        std::string s = s0;
        if (excel_protect && !s.empty() && (s[0] == '-' || s[0] == '+' || s[0] == '=' || s[0] == '@')) {
            s = "=\"" + s + "\"";
        }
        bool need_quote = s.find(',') != std::string::npos || s.find('"') != std::string::npos ||
                          s.find('\n') != std::string::npos || s.find('\r') != std::string::npos ||
                          (!s.empty() && (s[0] == ' ' || s.back() == ' '));
        if (!need_quote) return s;
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\"\"";
            else out.push_back(c);
        }
        out.push_back('"');
        return out;
    }

    static void ensure_clean_dir(const std::string& path) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }

    static void ensure_dir(const std::string& path) {
        std::error_code ec;
        fs::create_directories(path, ec);
    }

    static double wall_seconds_now() {
        using clock = std::chrono::steady_clock;
        static const auto base = clock::now();
        return std::chrono::duration<double>(clock::now() - base).count();
    }

    static std::string format_double(double x, int digits) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(digits) << x;
        return oss.str();
    }

    // 读取 Fluxnet 实验配置，并解析站点选择、时间范围、采样、窗口和推断参数。
    static std::vector<ExperimentSpec> load_experiment_specs() {
        std::vector<ExperimentSpec> specs;
        if (!fs::exists(kInputPath)) {
            ensure_dir("experiment");
            std::ofstream out(kInputPath, std::ios::binary | std::ios::trunc);
            out << "\xEF\xBB\xBF";
            out << "试验品个数或指定试验品集合,查询时间起止范围,选择分辨率,参与列,完整子随机过程长度T,推断时的τ,合并时的Δ,合并时的θ,MemTable容量(MB),是否测试故障数据集,是否打印过程日志,是否使用协方差缓存驱动CDiT\n";
            out << "1,\"=\"\"-∞到+∞\"\"\",按日,\"N,L,V\",30,5,2,0.8,2,是,是,是\n";
        }

        std::ifstream in(kInputPath, std::ios::binary);
        if (!in.good()) return specs;
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (first) {
                first = false;
                if (starts_with(line, "\xEF\xBB\xBF")) line = line.substr(3);
                continue;
            }
            if (trim(line).empty()) continue;
            std::vector<std::string> parts = split_csv_line(line);
            if (parts.size() < 11) continue;
            ExperimentSpec sp;
            sp.sample_spec = strip_excel_formula_text(parts[0]);
            sp.range = parse_time_range(parts[1]);
            sp.resolution_text = strip_excel_formula_text(parts[2]);
            sp.resolution_code = resolution_to_code(sp.resolution_text);
            sp.resolution_text = resolution_code_to_text(sp.resolution_code);
            for (std::string col : split_by_delims(strip_excel_formula_text(parts[3]))) {
                if (!trim(col).empty()) sp.selected_columns.push_back(trim(col));
            }
            sp.window_length = std::atoi(strip_excel_formula_text(parts[4]).c_str());
            sp.tau = std::atoi(strip_excel_formula_text(parts[5]).c_str());
            sp.delta = std::atoi(strip_excel_formula_text(parts[6]).c_str());
            sp.theta = std::atof(strip_excel_formula_text(parts[7]).c_str());
            sp.memtable_mb = std::atoi(strip_excel_formula_text(parts[8]).c_str());
            sp.test_fault = parse_bool_cn(parts[9]);
            sp.print_process_log = parse_bool_cn(parts[10]);
            sp.use_covariance_cache_cdit = parts.size() >= 12 ? parse_bool_cn(parts[11]) : false;
            if (sp.window_length <= 0) sp.window_length = 30;
            if (sp.tau <= 0) sp.tau = 5;
            if (sp.delta <= 0) sp.delta = 2;
            if (sp.memtable_mb <= 0) sp.memtable_mb = 2;
            if (sp.selected_columns.empty()) sp.selected_columns = {"R", "N", "T", "V", "H", "L", "P", "G"};
            specs.push_back(sp);
        }
        return specs;
    }

    // 根据站点、分辨率及故障后缀定位本轮需要加载的 Fluxnet 数据文件。
    static std::vector<DatasetFileInfo> build_dataset_files(const ExperimentSpec& sp, const std::string& fault_suffix) {
        std::vector<DatasetFileInfo> out;
        std::vector<std::string> folders = select_folders_with_completion(sp, fault_suffix);

        for (const std::string& folder : folders) {
            std::string folder_path = kDataRoot + "/" + folder;
            std::string base_file = find_file_for_suffix(folder_path, sp.resolution_code, "");
            if (base_file.empty()) continue;

            std::string run_file = fault_suffix.empty() ? base_file : find_file_for_suffix(folder_path, sp.resolution_code, fault_suffix);
            if (run_file.empty()) continue;

            DatasetFileInfo info;
            info.folder_path = folder_path;
            info.folder_name = folder;
            info.site_short = site_short_from_folder(folder);
            info.dataset_id = info.site_short;
            info.query_display_name = remove_pre_suffix_for_display(base_file);
            info.base_file_path = base_file;
            info.run_file_path = run_file;
            out.push_back(std::move(info));
        }

        return out;
    }

    static void write_summary_header() {
        ensure_dir(kLogDir);
        std::ofstream out(kSummaryPath, std::ios::binary | std::ios::trunc);
        out << "\xEF\xBB\xBF";
        out << "插入的时间范围,插入的试验品的数量,分辨率,插入时的T,推断时的τ,插入的列,合并时的Δ,合并时的θ,MemTable(MB),是否测试故障数据集,是否打印过程日志,是否使用协方差缓存驱动CDiT,插入耗时(s),合并压缩率,仅合并部分耗时(s),全数据集全跨度全列范围查询总耗时(s),库内推断算法部分耗时(s),故障程度,全部页面数,互补页面数,重叠页面数,插入总耗时减库内推断耗时(s)\n";
    }

    static void append_summary_row(const ExperimentSpec& sp,
                                   int dataset_count,
                                   const std::string& fault_label,
                                   double insert_seconds,
                                   double mcr,
                                   double query_seconds,
                                   const CausalMetaCounters& counters) {
        std::ofstream out(kSummaryPath, std::ios::binary | std::ios::app);
        std::vector<std::string> fields;
        fields.push_back(sp.range.raw_display);
        fields.push_back(std::to_string(dataset_count));
        fields.push_back(sp.resolution_text);
        fields.push_back(std::to_string(sp.window_length));
        fields.push_back(std::to_string(sp.tau));
        fields.push_back(join(sp.selected_columns, ","));
        fields.push_back(std::to_string(sp.delta));
        fields.push_back(format_double(sp.theta, 1));
        fields.push_back(std::to_string(sp.memtable_mb));
        fields.push_back(sp.test_fault ? "是" : "否");
        fields.push_back(sp.print_process_log ? "是" : "否");
        fields.push_back(sp.use_covariance_cache_cdit ? "是" : "否");
        fields.push_back(format_double(insert_seconds, 4));
        fields.push_back(format_double(mcr, 2) + "%");
        fields.push_back(format_double(counters.merge_seconds, 6));
        fields.push_back(format_double(query_seconds, 6));
        fields.push_back(format_double(counters.metadata_seconds, 6));
        fields.push_back(fault_label);
        fields.push_back(std::to_string(counters.total_pages));
        fields.push_back(std::to_string(counters.complementary_pages));
        fields.push_back(std::to_string(counters.overlapped_pages));
        fields.push_back(format_double(insert_seconds - counters.metadata_seconds, 6));
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i) out << ',';
            out << make_csv_field(fields[i]);
        }
        out << "\n";
        out.flush();
    }

    static std::string format_query_graph_ordered(const CDiTResult& graph, const std::vector<std::string>& names) {
        struct Item { int rp, rq; std::string ps, qs; std::vector<int> lags; };
        std::vector<Item> items;
        for (const auto& kv : graph.lags_by_edge) {
            if (kv.second.empty()) continue;
            int p = kv.first.first;
            int q = kv.first.second;
            if (p < 1 || q < 1 || p > (int)names.size() || q > (int)names.size()) continue;
            items.push_back({p - 1, q - 1, names[(size_t)p - 1], names[(size_t)q - 1], kv.second});
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.rp != b.rp) return a.rp < b.rp;
            return a.rq < b.rq;
        });
        std::ostringstream oss;
        for (const Item& it : items) {
            std::vector<int> lags = it.lags;
            std::sort(lags.begin(), lags.end());
            oss << it.ps << "→" << it.qs << "滞后=";
            for (int v : lags) oss << v << ',';
            oss << "; ";
        }
        return oss.str();
    }

    static double run_queries_and_log(const ExperimentSpec& sp,
                                      const std::vector<DatasetFileInfo>& datasets,
                                      ExperimentLogger& logger) {
        double total = 0.0;
        for (const DatasetFileInfo& ds : datasets) {
            double t0 = wall_seconds_now();
            std::vector<CausalMetaQuerySegment> segs = causal_meta_query_dataset_from_file(
                    kMetaDir, ds.dataset_id, "-∞", "+∞", sp.selected_columns);
            double t1 = wall_seconds_now();
            total += (t1 - t0);

            logger.write("本次范围查询输入：查询的试验品: " + ds.query_display_name + "；查询时间区间:-∞到+∞；查询列:" + join(sp.selected_columns, ",") + "\n");
            logger.write("查询结果如下\n");
            for (const CausalMetaQuerySegment& seg : segs) {
                logger.write(seg.start_label + "到" + seg.end_label + " | 推断：" +
                             format_query_graph_ordered(seg.graph, sp.selected_columns) + "\n");
            }
            logger.write("本次范围查询小节：耗时:" + format_double(t1 - t0, 6) + "s\n\n");
        }
        return total;
    }

    // 根据站点时间戳和实验参数注册元数据维护配置。
    static bool register_dataset_for_meta(const ExperimentSpec& sp, const DatasetFileInfo& ds) {
        std::vector<CausalMetaTimestampInfo> ordered = load_ordered_timestamps_from_base(ds.base_file_path, sp.range);
        if (ordered.empty()) return false;
        CausalMetaDatasetConfig cfg;
        cfg.dataset_id = ds.dataset_id;
        cfg.display_name = ds.query_display_name;
        cfg.variable_names = sp.selected_columns;
        cfg.ordered_timestamps = std::move(ordered);
        cfg.sampling_step = 1;
        cfg.window_length = sp.window_length;
        cfg.tau = sp.tau;
        cfg.delta = sp.delta;
        cfg.theta = sp.theta;
        cfg.alpha = 0.01;
        cfg.infer_self = false;
        cfg.ignore_self_in_merge = true;
        cfg.use_covariance_cache_cdit = sp.use_covariance_cache_cdit;
        cfg.metadata_dir = kMetaDir;
        return causal_meta_register_dataset(cfg);
    }

    // 将准备好的站点观测记录按数据库键值格式写入 RemixDB。
    static void insert_dataset_rows(struct xdb_ref* ref, const ExperimentSpec& sp, const DatasetFileInfo& ds) {
        std::vector<int> col_indices = selected_column_indices_from_file(ds.run_file_path, sp.selected_columns);
        if ((int)col_indices.size() != (int)sp.selected_columns.size()) return;
        std::vector<CsvDataRow> rows = read_rows_for_insert(ds.run_file_path, sp.range, col_indices);
        for (const CsvDataRow& row : rows) {
            std::string key = ds.dataset_id + "," + row.timestamp_raw;
            std::string val = value_string_from_fields(row.fields);
            remixdb_put(ref, key.data(), (u32)key.size(), val.data(), (u32)val.size());
        }
    }

    // 执行单个 Fluxnet 配置及故障版本，完成数据写入、元数据刷新、范围查询和统计记录。
    static int run_one_experiment(int experiment_index,
                                  const ExperimentSpec& sp,
                                  const std::string& fault_suffix,
                                  ExperimentLogger& logger) {
        const std::string fault_label = fault_suffix.empty() ? "无" : fault_suffix;
        ensure_clean_dir(kMetaDir);
        ensure_clean_dir(kXdbDir);
        causal_meta_reset_all();
        causal_meta_set_enabled_cpp(true);
        causal_meta_set_metadata_dir(kMetaDir);
        causal_meta_set_page_logging(sp.print_process_log);
        LogSinkContext sink_ctx{&logger};
        if (sp.print_process_log) causal_meta_set_log_sink(meta_log_sink, &sink_ctx);
        else causal_meta_clear_log_sink();

        std::vector<DatasetFileInfo> datasets = build_dataset_files(sp, fault_suffix);
        logger.write("第" + std::to_string(experiment_index) + "次实验\n");
        logger.write("数据库参数设置：MemTable=" + std::to_string(sp.memtable_mb) + "MB\n");
        logger.write("本次插入实验参数设置：插入时间范围=" + sp.range.raw_display + "；要插入的试验品个数=" +
                     std::to_string((int)datasets.size()) + "；分辨率=" + sp.resolution_text + "；插入时参与列=" +
                     join(sp.selected_columns, ",") + "；T=" + std::to_string(sp.window_length) + "；τ=" +
                     std::to_string(sp.tau) + "；Δ=" + std::to_string(sp.delta) + "；θ=" + format_double(sp.theta, 1) +
                     "；是否使用协方差缓存驱动CDiT=" + (sp.use_covariance_cache_cdit ? "是" : "否") +
                     "；故障程度=" + fault_label + "\n\n");

        for (const DatasetFileInfo& ds : datasets) register_dataset_for_meta(sp, ds);

        struct xdb* db = remixdb_open(kXdbDir.c_str(), 128, (size_t)sp.memtable_mb, true);
        if (!db) {
            logger.write("数据库启动失败，本次实验跳过。\n\n");
            return experiment_index + 1;
        }
        struct xdb_ref* ref = remixdb_ref(db);

        double t0 = wall_seconds_now();
        for (const DatasetFileInfo& ds : datasets) insert_dataset_rows(ref, sp, ds);
        remixdb_unref(ref);
        remixdb_causal_meta_flush(db);
        double t1 = wall_seconds_now();

        CausalMetaCounters counters = causal_meta_get_counters();
        double mcr = 0.0;
        if (counters.original_complete_windows > 0) {
            mcr = ((double)counters.original_complete_windows - (double)counters.final_metadata_entries) /
                  (double)counters.original_complete_windows * 100.0;
            if (mcr < 0.0) mcr = 0.0;
        }
        logger.write("本次插入完毕 | 耗时：" + format_double(t1 - t0, 4) +
                     "s；合并压缩率：" + format_double(mcr, 2) +
                     "%；仅合并部分耗时：" + format_double(counters.merge_seconds, 6) + "s\n\n");

        double query_seconds = run_queries_and_log(sp, datasets, logger);
        counters = causal_meta_get_counters();
        append_summary_row(sp, (int)datasets.size(), fault_label, t1 - t0, mcr, query_seconds, counters);

        remixdb_close(db);
        causal_meta_clear_log_sink();
        causal_meta_reset_all();
        ensure_clean_dir(kXdbDir);
        ensure_clean_dir(kMetaDir);
        return experiment_index + 1;
    }

} // 匿名命名空间结束

// Fluxnet 实验总入口：加载配置，发现可用故障版本，并按配置顺序运行各组实验。
int run_fluxnet_experiments() {
    ensure_dir(kLogDir);
    write_summary_header();
    {
        std::ofstream clear(kDetailLogPath, std::ios::binary | std::ios::trunc);
        clear << "\xEF\xBB\xBF";
    }
    ExperimentLogger logger(kDetailLogPath);

    std::vector<ExperimentSpec> specs = load_experiment_specs();
    if (specs.empty()) {
        logger.write("未找到可执行的生物圈大气通量实验参数。\n");
        return 1;
    }

    int experiment_index = 1;
    for (const ExperimentSpec& sp : specs) {
        std::vector<std::string> folders = folders_for_fault_suffix_discovery(sp);
        std::vector<std::string> suffixes;
        suffixes.push_back("");

        if (sp.test_fault) {
            std::set<std::string> discovered = discover_fault_suffixes(folders, sp.resolution_code);
            suffixes.insert(suffixes.end(), discovered.begin(), discovered.end());
        }

        for (const std::string& suffix : suffixes) {
            experiment_index = run_one_experiment(experiment_index, sp, suffix, logger);
        }
    }

    logger.write("生物圈大气通量全部实验完成。\n");
    return 0;
}
