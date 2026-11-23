/*
 * 本文件实现 Fluxnet 数据集的库外处理对照实验，负责数据加载、窗口分析、结果组织和耗时统计。
 */

#include "outdb_fluxnet_experiment.h"

#include "../cdit_module.h"
#include "../merge_cg.h"
#include "../xdb.h"

#if __has_include("../causal_meta.h")
#include "../causal_meta.h"
#define OUTDB_FLUXNET_HAS_CAUSAL_META 1
#else
#define OUTDB_FLUXNET_HAS_CAUSAL_META 0
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

    const char* kDataRoot = "data/fluxnet_data";
    const char* kDbDir = "xdb_fluxnet_outdb_experiment";
    const char* kInputCsv = "experiment/outdb_fluxnet_experiment_input.csv";
    const char* kLogDir = "experiment_log";
    const char* kStatCsv = "experiment_log/对生物圈大气通量的入库+库外推断实验统计.csv";
    const char* kPrintLog = "experiment_log/生物圈大气通量入库+库外推断实验全部打印日志.txt";

    struct Logger {
        std::ofstream fout;

        explicit Logger(const std::string& path) {
            fout.open(path, std::ios::binary | std::ios::trunc);
            const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            fout.write(reinterpret_cast<const char*>(bom), 3);
        }

        template <typename T>
        Logger& operator<<(const T& x) {
            std::cout << x;
            if (fout.good()) fout << x;
            return *this;
        }

        typedef std::ostream& (*StdManip)(std::ostream&);
        Logger& operator<<(StdManip manip) {
            manip(std::cout);
            if (fout.good()) manip(fout);
            return *this;
        }

        void flush() {
            std::cout.flush();
            if (fout.good()) fout.flush();
        }
    };

    struct ExperimentConfig {
        std::string raw_selector;
        std::string raw_time_range;
        std::string resolution_display;
        std::string resolution_code;
        std::vector<std::string> requested_aliases;
        int window_length = 30;
        int tau = 5;
        int delta = 2;
        double theta = 0.8;
        int memtable_mb = 2;
        int sampling_step = 1;
        bool use_covmeta_cdit = false;
    };

    struct FluxnetFile {
        std::string site_id;
        std::string file_id;
        std::filesystem::path path;
    };

    struct FluxnetRow {
        std::string timestamp_key;
        std::string label;
        long long comparable = 0;
        std::string value_wire;
    };

    struct PreparedSample {
        FluxnetFile file;
        std::vector<std::string> selected_aliases;
        std::vector<int> selected_csv_indexes;
        std::vector<FluxnetRow> rows;
    };

    struct Segment {
        int start_row_index = 0;
        int end_row_index = 0;
        std::string start_label;
        std::string end_label;
        CDiTResult graph;
    };

    struct SampleInferenceResult {
        std::vector<Segment> segments;
        long long original_windows = 0;
    };

    struct ExperimentStats {
        double total_seconds = 0.0;
        double insert_seconds = 0.0;
        double range_section_seconds = 0.0;
        double read_seconds = 0.0;
        double infer_seconds = 0.0;
        double merge_seconds = 0.0;
        long long original_windows = 0;
        long long final_segments = 0;
    };

    static double now_seconds() {
        using clock = std::chrono::steady_clock;
        return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    static std::string trim(const std::string& s) {
        size_t l = 0;
        size_t r = s.size();
        while (l < r && std::isspace(static_cast<unsigned char>(s[l]))) ++l;
        while (r > l && std::isspace(static_cast<unsigned char>(s[r - 1]))) --r;
        return s.substr(l, r - l);
    }

    static bool ends_with(const std::string& s, const std::string& p) {
        return s.size() >= p.size() && std::equal(p.rbegin(), p.rend(), s.rbegin());
    }

    static std::string strip_outer_quotes(std::string s) {
        s = trim(s);
        bool changed = true;
        while (changed) {
            changed = false;
            s = trim(s);
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                s = s.substr(1, s.size() - 2);
                changed = true;
            }
        }
        return trim(s);
    }

    static std::string normalize_excel_cell(std::string s) {
        s = trim(s);
        if (!s.empty() && s.front() == '=') s = trim(s.substr(1));
        s = strip_outer_quotes(s);
        return trim(s);
    }

    static bool parse_yes_no(const std::string& raw, bool default_value = false) {
        std::string s = normalize_excel_cell(raw);
        std::string lower;
        lower.reserve(s.size());
        for (unsigned char c : s) lower.push_back(static_cast<char>(std::tolower(c)));
        if (s == "是" || lower == "true" || lower == "yes" || lower == "y" || lower == "1" || lower == "on") return true;
        if (s == "否" || lower == "false" || lower == "no" || lower == "n" || lower == "0" || lower == "off") return false;
        return default_value;
    }

    static const char* yes_no(bool x) {
        return x ? "是" : "否";
    }

    static std::string normalize_infinity_range_for_display(const std::string& raw) {
        return normalize_excel_cell(raw);
    }

    static std::vector<std::string> parse_csv_line(const std::string& line) {
        std::vector<std::string> out;
        std::string cur;
        bool in_quotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
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

    static std::string csv_escape(const std::string& s) {
        bool need = false;
        for (char c : s) {
            if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                need = true;
                break;
            }
        }
        if (!s.empty() && (s.front() == '=' || s.front() == '-' || s.front() == '+' || s.front() == '@')) need = true;
        if (!need) return s;
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\"\"";
            else out.push_back(c);
        }
        out += '"';
        return out;
    }

    static std::string excel_formula_safe_if_needed(const std::string& s) {
        std::string x = normalize_excel_cell(s);
        if (!x.empty() && (x.front() == '-' || x.front() == '=' || x.front() == '+' || x.front() == '@')) return "=\"" + x + "\"";
        if (x.find("-∞") != std::string::npos) return "=\"" + x + "\"";
        return x;
    }

    static void write_csv_row(std::ofstream& out, const std::vector<std::string>& fields) {
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i) out << ',';
            out << csv_escape(fields[i]);
        }
        out << '\n';
        out.flush();
    }

    static std::vector<std::string> split_tokens(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (std::isspace(c) || s[i] == ',' || s[i] == ';') {
                if (!cur.empty()) {
                    out.push_back(cur);
                    cur.clear();
                }
            } else cur.push_back(s[i]);
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    static std::vector<std::string> split_alias_list(const std::string& s) {
        std::vector<std::string> xs = split_tokens(normalize_excel_cell(s));
        for (std::string& x : xs) x = trim(x);
        xs.erase(std::remove_if(xs.begin(), xs.end(), [](const std::string& x) { return x.empty(); }), xs.end());
        return xs;
    }

    static bool is_integer_string(const std::string& s0) {
        std::string s = trim(s0);
        if (s.empty()) return false;
        for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        return true;
    }

    static std::string resolution_code_from_display(const std::string& raw) {
        std::string r = normalize_excel_cell(raw);
        if (r == "按半小时" || r == "半小时" || r == "HH" || r == "hh") return "HH";
        if (r == "按日" || r == "日" || r == "DD" || r == "dd") return "DD";
        if (r == "按周" || r == "周" || r == "WW" || r == "ww") return "WW";
        if (r == "按月" || r == "月" || r == "MM" || r == "mm") return "MM";
        if (r == "按年" || r == "年" || r == "YY" || r == "yy") return "YY";
        return r;
    }

    static std::string resolution_display_from_code(const std::string& code) {
        if (code == "HH") return "按半小时";
        if (code == "DD") return "按日";
        if (code == "WW") return "按周";
        if (code == "MM") return "按月";
        if (code == "YY") return "按年";
        return code;
    }

    static std::string alias_to_csv_column(const std::string& alias) {
        if (alias == "R") return "SW_IN_F_MDS";
        if (alias == "N") return "NEE_VUT_USTAR50";
        if (alias == "T") return "TA_F_MDS";
        if (alias == "V") return "VPD_F_MDS";
        if (alias == "H") return "H_F_MDS";
        if (alias == "L") return "LE_F_MDS";
        if (alias == "P") return "P_F";
        if (alias == "G") return "GPP_NT_VUT_USTAR50";
        return alias;
    }

    static std::string join_aliases(const std::vector<std::string>& xs) {
        std::ostringstream oss;
        for (size_t i = 0; i < xs.size(); ++i) {
            if (i) oss << ',';
            oss << xs[i];
        }
        return oss.str();
    }

    static std::string site_id_from_name(const std::string& name) {
        std::string s = name;
        size_t p = s.find("FLX_");
        if (p != std::string::npos) s = s.substr(p + 4);
        size_t q = s.find("_FLUXNET");
        if (q != std::string::npos) s = s.substr(0, q);
        return s;
    }

    static std::string file_id_from_path(const std::filesystem::path& p) {
        std::string stem = p.filename().string();
        if (ends_with(stem, ".csv")) stem = stem.substr(0, stem.size() - 4);
        if (ends_with(stem, "_pre")) stem = stem.substr(0, stem.size() - 4);
        return stem;
    }

    static std::string digits_only(const std::string& s) {
        std::string d;
        for (char c : s) if (std::isdigit(static_cast<unsigned char>(c))) d.push_back(c);
        return d;
    }

    static long long make_comparable(int y, int mo, int da, int hh, int mm) {
        return (long long)y * 100000000LL + (long long)mo * 1000000LL + (long long)da * 10000LL + (long long)hh * 100LL + (long long)mm;
    }

    static long long parse_timestamp_comparable(const std::string& raw) {
        std::string d = digits_only(raw);
        int y = 0, mo = 1, da = 1, hh = 0, mm = 0;
        if (d.size() >= 4) y = std::atoi(d.substr(0, 4).c_str());
        if (d.size() >= 6) mo = std::atoi(d.substr(4, 2).c_str());
        if (d.size() >= 8) da = std::atoi(d.substr(6, 2).c_str());
        if (d.size() >= 10) hh = std::atoi(d.substr(8, 2).c_str());
        if (d.size() >= 12) mm = std::atoi(d.substr(10, 2).c_str());
        return make_comparable(y, mo, da, hh, mm);
    }

    static std::string label_from_timestamp(const std::string& raw, const std::string& resolution_code) {
        std::string d = digits_only(raw);
        int y = 0, mo = 1, da = 1, hh = 0, mm = 0;
        if (d.size() >= 4) y = std::atoi(d.substr(0, 4).c_str());
        if (d.size() >= 6) mo = std::atoi(d.substr(4, 2).c_str());
        if (d.size() >= 8) da = std::atoi(d.substr(6, 2).c_str());
        if (d.size() >= 10) hh = std::atoi(d.substr(8, 2).c_str());
        if (d.size() >= 12) mm = std::atoi(d.substr(10, 2).c_str());
        if (resolution_code == "MM") da = 1;
        if (resolution_code == "YY") { mo = 1; da = 1; }
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(4) << y << '-' << std::setw(2) << mo << '-' << std::setw(2) << da;
        oss << std::setfill(' ') << ' ' << hh << ':' << mm;
        return oss.str();
    }

    static std::pair<long long, long long> parse_time_range(const std::string& raw) {
        std::string s = normalize_excel_cell(raw);
        const long long neg_inf = std::numeric_limits<long long>::min() / 4;
        const long long pos_inf = std::numeric_limits<long long>::max() / 4;
        size_t p = s.find("到");
        if (p == std::string::npos) {
            std::string x = trim(s);
            if (x == "-∞" || x == "-") return std::make_pair(neg_inf, pos_inf);
            return std::make_pair(parse_timestamp_comparable(x), pos_inf);
        }
        std::string a = trim(s.substr(0, p));
        std::string b = trim(s.substr(p + std::string("到").size()));
        long long lo = (a == "-∞" || a == "-") ? neg_inf : parse_timestamp_comparable(a);
        long long hi = (b == "+∞" || b == "+") ? pos_inf : parse_timestamp_comparable(b);
        return std::make_pair(lo, hi);
    }

    static std::vector<FluxnetFile> discover_fluxnet_files(const std::string& resolution_code) {
        std::vector<FluxnetFile> files;
        std::filesystem::path root(kDataRoot);
        if (!std::filesystem::exists(root)) return files;
        auto consider_file = [&](const std::filesystem::path& p) {
            if (!std::filesystem::is_regular_file(p)) return;
            std::string fn = p.filename().string();
            std::string needle = "_" + resolution_code + "_";
            if (fn.find(needle) == std::string::npos || !ends_with(fn, "_pre.csv")) return;
            FluxnetFile f;
            f.site_id = site_id_from_name(fn);
            // ZA-Kru 属于 FLUXNET2015 Tier Two，不纳入本项目公开数据范围。
            if (f.site_id == "ZA-Kru") return;
            f.file_id = file_id_from_path(p);
            f.path = p;
            files.push_back(std::move(f));
        };
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_regular_file()) consider_file(entry.path());
            else if (entry.is_directory()) for (const auto& sub : std::filesystem::directory_iterator(entry.path())) consider_file(sub.path());
        }
        std::sort(files.begin(), files.end(), [](const FluxnetFile& a, const FluxnetFile& b) {
            if (a.site_id != b.site_id) return a.site_id < b.site_id;
            return a.path.string() < b.path.string();
        });
        files.erase(std::unique(files.begin(), files.end(), [](const FluxnetFile& a, const FluxnetFile& b) {
            return a.site_id == b.site_id && a.path == b.path;
        }), files.end());
        return files;
    }

    static std::vector<FluxnetFile> select_fluxnet_files(const std::vector<FluxnetFile>& all, const std::string& selector_raw) {
        std::string selector = normalize_excel_cell(selector_raw);
        std::vector<FluxnetFile> chosen;
        if (all.empty()) return chosen;
        if (is_integer_string(selector)) {
            int n = std::atoi(selector.c_str());
            if (n <= 0) return chosen;
            if (n >= (int)all.size()) return all;
            std::set<int> used;
            if (n == 1) used.insert(0);
            else {
                for (int i = 0; i < n; ++i) {
                    int idx = (int)std::llround((double)i * (double)(all.size() - 1) / (double)(n - 1));
                    idx = std::max(0, std::min(idx, (int)all.size() - 1));
                    used.insert(idx);
                }
                for (int idx = 0; (int)used.size() < n && idx < (int)all.size(); ++idx) used.insert(idx);
            }
            for (int idx : used) chosen.push_back(all[(size_t)idx]);
            return chosen;
        }
        std::vector<std::string> tokens = split_tokens(selector);
        std::set<std::string> seen;
        for (const std::string& tok0 : tokens) {
            std::string normalized = site_id_from_name(trim(tok0));
            for (const FluxnetFile& f : all) {
                if (f.site_id == normalized && !seen.count(f.path.string())) {
                    chosen.push_back(f);
                    seen.insert(f.path.string());
                    break;
                }
            }
        }
        return chosen;
    }

    static bool read_header_and_indexes(const FluxnetFile& file,
                                        const std::vector<std::string>& aliases,
                                        std::vector<int>& indexes,
                                        std::vector<std::string>& header) {
        std::ifstream in(file.path);
        if (!in.good()) return false;
        std::string line;
        if (!std::getline(in, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        header = parse_csv_line(line);
        std::map<std::string, int> pos;
        for (size_t i = 0; i < header.size(); ++i) pos[trim(header[i])] = (int)i;
        indexes.clear();
        for (const std::string& alias : aliases) {
            auto it = pos.find(alias_to_csv_column(alias));
            if (it == pos.end()) return false;
            indexes.push_back(it->second);
        }
        return true;
    }

    static std::string build_value_wire(const std::vector<std::string>& fields, const std::vector<int>& indexes) {
        std::ostringstream oss;
        for (size_t i = 0; i < indexes.size(); ++i) {
            if (i) oss << ',';
            int idx = indexes[i];
            if (idx >= 0 && idx < (int)fields.size()) oss << trim(fields[(size_t)idx]);
            else oss << '0';
        }
        return oss.str();
    }

    static PreparedSample load_sample_metadata(const FluxnetFile& file,
                                               const std::vector<std::string>& aliases,
                                               const std::pair<long long,long long>& range,
                                               const std::string& resolution_code,
                                               Logger& logger) {
        PreparedSample sample;
        sample.file = file;
        sample.selected_aliases = aliases;
        std::vector<std::string> header;
        if (!read_header_and_indexes(file, aliases, sample.selected_csv_indexes, header)) {
            logger << "警告：无法读取或匹配列，跳过试验品：" << file.path.string() << "\n";
            return sample;
        }
        std::ifstream in(file.path);
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (trim(line).empty()) continue;
            std::vector<std::string> fields = parse_csv_line(line);
            if (fields.empty()) continue;
            std::string ts = trim(fields[0]);
            long long comp = parse_timestamp_comparable(ts);
            if (comp < range.first || comp > range.second) continue;
            FluxnetRow row;
            row.timestamp_key = ts;
            row.label = label_from_timestamp(ts, resolution_code);
            row.comparable = comp;
            row.value_wire = build_value_wire(fields, sample.selected_csv_indexes);
            sample.rows.push_back(std::move(row));
        }
        return sample;
    }

    static std::string db_key(const PreparedSample& sample, const FluxnetRow& row) {
        return sample.file.file_id + "," + row.timestamp_key;
    }

    static void clean_db_dir() {
        std::filesystem::remove_all(kDbDir);
        std::filesystem::create_directories(kDbDir);
    }

    static std::vector<double> parse_value_vector(const char* buf, u32 len, int expected_d) {
        std::vector<double> vals;
        vals.reserve((size_t)expected_d);
        const char* p = buf;
        const char* end = buf + len;
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            char* q = nullptr;
            double v = std::strtod(p, &q);
            if (q == p) {
                v = 0.0;
                q = const_cast<char*>(p);
                while (q < end && *q != ',') ++q;
            }
            vals.push_back(v);
            p = q;
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            if (p < end && *p == ',') ++p;
        }
        while ((int)vals.size() < expected_d) vals.push_back(0.0);
        if ((int)vals.size() > expected_d) vals.resize((size_t)expected_d);
        return vals;
    }

    static std::string format_graph_ordered(const CDiTResult& graph, const std::vector<std::string>& aliases) {
        struct Item { int p; int q; std::vector<int> lags; };
        std::vector<Item> items;
        for (const auto& kv : graph.lags_by_edge) {
            if (kv.second.empty()) continue;
            Item item{kv.first.first, kv.first.second, kv.second};
            std::sort(item.lags.begin(), item.lags.end());
            items.push_back(std::move(item));
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.p != b.p) return a.p < b.p;
            return a.q < b.q;
        });
        auto name_of = [&](int idx) -> std::string {
            if (idx >= 1 && idx <= (int)aliases.size()) return aliases[(size_t)idx - 1];
            return "X" + std::to_string(idx);
        };
        std::ostringstream oss;
        for (const Item& item : items) {
            oss << name_of(item.p) << "→" << name_of(item.q) << "滞后=";
            for (int v : item.lags) oss << v << ',';
            oss << "; ";
        }
        return oss.str();
    }

    static bool try_merge_segments(const Segment& left,
                                   const Segment& right,
                                   const ExperimentConfig& cfg,
                                   Segment& merged,
                                   double& merge_seconds) {
        const double merge_t0 = now_seconds();
        MergeCGResult mr = merge_adjacent_causal_graphs(left.graph, right.graph, cfg.tau, cfg.delta, cfg.theta, true);
        merge_seconds += now_seconds() - merge_t0;

        if (!mr.merge) return false;
        merged.start_row_index = left.start_row_index;
        merged.end_row_index = right.end_row_index;
        merged.start_label = left.start_label;
        merged.end_label = right.end_label;
        merged.graph = std::move(mr.merged);
        return true;
    }

    static void insert_segment_with_rollback_merge(std::vector<Segment>& segs,
                                                   Segment seg,
                                                   const ExperimentConfig& cfg,
                                                   double& merge_seconds) {
        auto it = std::lower_bound(segs.begin(), segs.end(), seg.start_row_index,
                                   [](const Segment& s, int start) { return s.start_row_index < start; });
        size_t idx = (size_t)(it - segs.begin());
        segs.insert(it, std::move(seg));
        bool changed = true;
        while (changed && idx < segs.size()) {
            changed = false;
            if (idx > 0) {
                Segment merged;
                if (segs[idx - 1].end_row_index + 1 == segs[idx].start_row_index &&
                    try_merge_segments(segs[idx - 1], segs[idx], cfg, merged, merge_seconds)) {
                    segs.erase(segs.begin() + (long)idx - 1, segs.begin() + (long)idx + 1);
                    segs.insert(segs.begin() + (long)idx - 1, std::move(merged));
                    --idx;
                    changed = true;
                    continue;
                }
            }
            if (idx + 1 < segs.size()) {
                Segment merged;
                if (segs[idx].end_row_index + 1 == segs[idx + 1].start_row_index &&
                    try_merge_segments(segs[idx], segs[idx + 1], cfg, merged, merge_seconds)) {
                    segs.erase(segs.begin() + (long)idx, segs.begin() + (long)idx + 2);
                    segs.insert(segs.begin() + (long)idx, std::move(merged));
                    changed = true;
                }
            }
        }
    }

    static CDiTResult run_selected_cdit(const std::vector<std::vector<double>>& data,
                                        const ExperimentConfig& cfg) {
        if (cfg.use_covmeta_cdit) {
            return run_cdit_covmeta(data, cfg.tau, 0.01, false, nullptr, nullptr);
        }
        return run_cdit_original(data, cfg.tau, 0.01, false, nullptr, nullptr);
    }

    // 从 RemixDB 读取一个样本的窗口数据，执行因果推断并返回窗口级结果。
    static SampleInferenceResult infer_one_sample_from_db(struct xdb_ref* ref,
                                                          const PreparedSample& sample,
                                                          const ExperimentConfig& cfg,
                                                          double& read_seconds,
                                                          double& infer_seconds,
                                                          double& merge_seconds,
                                                          Logger& logger) {
        SampleInferenceResult result;
        const int d = (int)sample.selected_aliases.size();
        if (cfg.window_length <= 0 || d <= 0) return result;
        const int n = (int)sample.rows.size();
        const int complete_windows = n / cfg.window_length;
        result.original_windows = complete_windows;
        for (int w = 0; w < complete_windows; ++w) {
            const int begin = w * cfg.window_length;
            const int end = begin + cfg.window_length - 1;
            std::vector<std::vector<double>> data;
            data.reserve((size_t)cfg.window_length);
            bool ok = true;
            const double read_t0 = now_seconds();
            for (int i = begin; i <= end; ++i) {
                const std::string key = db_key(sample, sample.rows[(size_t)i]);
                char vbuf[65536];
                u32 vlen = 0;
                bool found = remixdb_get(ref, key.data(), (u32)key.size(), vbuf, &vlen);
                if (!found) { ok = false; break; }
                data.push_back(parse_value_vector(vbuf, vlen, d));
            }
            read_seconds += now_seconds() - read_t0;
            if (!ok || (int)data.size() != cfg.window_length) {
                logger << "警告：数据库读取窗口失败，跳过 " << sample.file.file_id << " 的 "
                       << sample.rows[(size_t)begin].label << " 至 " << sample.rows[(size_t)end].label << "\n";
                continue;
            }

            const double infer_t0 = now_seconds();
            CDiTResult graph = run_selected_cdit(data, cfg);
            infer_seconds += now_seconds() - infer_t0;

            Segment seg;
            seg.start_row_index = begin;
            seg.end_row_index = end;
            seg.start_label = sample.rows[(size_t)begin].label;
            seg.end_label = sample.rows[(size_t)end].label;
            seg.graph = std::move(graph);
            insert_segment_with_rollback_merge(result.segments, std::move(seg), cfg, merge_seconds);
        }
        return result;
    }

    // 读取 Fluxnet 库外对照实验配置，并解析站点、时间范围、窗口和推断参数。
    static std::vector<ExperimentConfig> load_experiment_configs(Logger& logger) {
        std::vector<ExperimentConfig> configs;
        std::ifstream in(kInputCsv, std::ios::binary);
        if (!in.good()) {
            logger << "警告：未找到 " << kInputCsv << "，将使用一个默认实验配置。\n";
            ExperimentConfig cfg;
            cfg.raw_selector = "1";
            cfg.raw_time_range = "-∞到+∞";
            cfg.resolution_display = "按日";
            cfg.resolution_code = "DD";
            cfg.requested_aliases = {"R","N","T","V","H","L","P","G"};
            cfg.use_covmeta_cdit = false;
            configs.push_back(std::move(cfg));
            return configs;
        }
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (trim(line).empty()) continue;
            std::vector<std::string> f = parse_csv_line(line);
            if (first) {
                first = false;
                if (!f.empty() && f[0].find("试验品") != std::string::npos) continue;
            }
            if (f.size() < 9) {
                logger << "警告：实验配置行字段不足，跳过：" << line << "\n";
                continue;
            }
            ExperimentConfig cfg;
            cfg.raw_selector = normalize_excel_cell(f[0]);
            cfg.raw_time_range = normalize_excel_cell(f[1]);
            cfg.resolution_code = resolution_code_from_display(f[2]);
            cfg.resolution_display = resolution_display_from_code(cfg.resolution_code);
            cfg.requested_aliases = split_alias_list(f[3]);
            cfg.window_length = std::atoi(normalize_excel_cell(f[4]).c_str());
            cfg.tau = std::atoi(normalize_excel_cell(f[5]).c_str());
            cfg.delta = std::atoi(normalize_excel_cell(f[6]).c_str());
            cfg.theta = std::atof(normalize_excel_cell(f[7]).c_str());
            cfg.memtable_mb = std::atoi(normalize_excel_cell(f[8]).c_str());
            cfg.use_covmeta_cdit = (f.size() >= 10) ? parse_yes_no(f[9], false) : false;
            if (cfg.window_length <= 0) cfg.window_length = 30;
            if (cfg.tau <= 0) cfg.tau = 5;
            if (cfg.delta <= 0) cfg.delta = 2;
            if (cfg.theta <= 0.0) cfg.theta = 0.8;
            if (cfg.memtable_mb <= 0) cfg.memtable_mb = 2;
            if (cfg.requested_aliases.empty()) cfg.requested_aliases = {"R","N","T","V","H","L","P","G"};
            configs.push_back(std::move(cfg));
        }
        return configs;
    }

    // 将准备好的 Fluxnet 样本按时间戳编码后写入 RemixDB。
    static bool insert_samples_to_db(const std::vector<PreparedSample>& samples, const ExperimentConfig& cfg, Logger& logger) {
#if OUTDB_FLUXNET_HAS_CAUSAL_META
        causal_meta_set_enabled(0);
#endif
        struct xdb* db = remixdb_open(kDbDir, 256, (size_t)cfg.memtable_mb, true);
        if (!db) {
            logger << "错误：无法打开 RemixDB：" << kDbDir << "\n";
            return false;
        }
        struct xdb_ref* ref = remixdb_ref(db);
        if (!ref) {
            remixdb_close(db);
            logger << "错误：无法创建 RemixDB ref。\n";
            return false;
        }
        for (const PreparedSample& sample : samples) {
            for (const FluxnetRow& row : sample.rows) {
                std::string key = db_key(sample, row);
                const std::string& value = row.value_wire;
                if (!remixdb_put(ref, key.data(), (u32)key.size(), value.data(), (u32)value.size())) {
                    logger << "警告：写入失败，key=" << key << "\n";
                }
            }
        }
        remixdb_sync(ref);
        remixdb_unref(ref);
        remixdb_close(db);
        return true;
    }

    static void write_stat_header(std::ofstream& stat) {
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        stat.write(reinterpret_cast<const char*>(bom), 3);
        write_csv_row(stat, {
                "试验品个数或指定试验品集合", "查询时间起止范围", "选择分辨率", "参与列",
                "完整子随机过程长度T", "推断时的τ", "合并时的Δ", "合并时的θ", "MemTable容量(MB)",
                "是否使用协方差缓存驱动CDiT", "总耗时(s)", "插入总耗时(s)", "合并压缩率",
                "库外推断中仅读取部分耗时(s)", "库外推断中仅推断部分耗时(s)", "仅合并部分耗时(s)"
        });
    }

    static std::string fmt_double_compact(double x) {
        std::ostringstream oss;
        oss << std::setprecision(12) << x;
        std::string out = oss.str();
        if (out.find('.') != std::string::npos) {
            while (!out.empty() && out.back() == '0') out.pop_back();
            if (!out.empty() && out.back() == '.') out.pop_back();
        }
        return out.empty() ? "0" : out;
    }

    static std::string fmt_seconds(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4) << x;
        return oss.str();
    }

    static std::string fmt_percent(double x) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << x << '%';
        return oss.str();
    }

    // 执行一组 Fluxnet 库外对照实验，记录数据准备、写入、读取和推断的分项耗时。
    static ExperimentStats run_one_experiment(int exp_id,
                                              const ExperimentConfig& cfg,
                                              std::ofstream& stat,
                                              Logger& logger) {
        ExperimentStats stats;
        clean_db_dir();
        const double total_t0 = now_seconds();
        const std::string time_display = normalize_infinity_range_for_display(cfg.raw_time_range);
        const std::string aliases_display = join_aliases(cfg.requested_aliases);
        const std::pair<long long,long long> range = parse_time_range(cfg.raw_time_range);
        std::vector<FluxnetFile> all_files = discover_fluxnet_files(cfg.resolution_code);
        std::vector<FluxnetFile> chosen_files = select_fluxnet_files(all_files, cfg.raw_selector);

        logger << "生物圈大气通量第" << exp_id << "次实验\n";
        logger << "数据库参数设置：MemTable=" << cfg.memtable_mb << "MB\n";
        logger << "本此插入实验参数设置：插入时间范围=" << time_display
               << "；要插入的试验品个数=" << chosen_files.size()
               << "；分辨率=" << cfg.resolution_display
               << "；是否使用协方差缓存驱动CDiT：" << yes_no(cfg.use_covmeta_cdit) << "；\n\n";

        if (chosen_files.empty()) {
            logger << "警告：未发现符合条件的 FLUXNET 文件，本次实验跳过。\n\n";
            stats.total_seconds = now_seconds() - total_t0;
            write_csv_row(stat, {
                    normalize_excel_cell(cfg.raw_selector), excel_formula_safe_if_needed(cfg.raw_time_range), cfg.resolution_display,
                    aliases_display, std::to_string(cfg.window_length), std::to_string(cfg.tau), std::to_string(cfg.delta),
                    fmt_double_compact(cfg.theta), std::to_string(cfg.memtable_mb), yes_no(cfg.use_covmeta_cdit),
                    fmt_seconds(stats.total_seconds), fmt_seconds(0.0), fmt_percent(0.0),
                    fmt_seconds(0.0), fmt_seconds(0.0), fmt_seconds(0.0)
            });
            return stats;
        }

        std::vector<PreparedSample> samples;
        samples.reserve(chosen_files.size());
        for (const FluxnetFile& f : chosen_files) {
            PreparedSample s = load_sample_metadata(f, cfg.requested_aliases, range, cfg.resolution_code, logger);
            if (!s.rows.empty()) samples.push_back(std::move(s));
            else logger << "警告：试验品在指定时间范围内无可插入数据，跳过：" << f.file_id << "\n";
        }

        const double insert_t0 = now_seconds();
        bool inserted = insert_samples_to_db(samples, cfg, logger);
        stats.insert_seconds = now_seconds() - insert_t0;

        logger << "本次范围推断输入：试验品个数=" << samples.size()
               << "；因果查询起止=" << time_display
               << "；分辨率=" << cfg.resolution_display
               << "；参与列=" << aliases_display
               << "；采样步长=" << cfg.sampling_step
               << "；T=" << cfg.window_length
               << "；τ=" << cfg.tau
               << "；Δ=" << cfg.delta
               << "；θ=" << cfg.theta << "\n";

        const double section_t0 = now_seconds();
        if (inserted) {
#if OUTDB_FLUXNET_HAS_CAUSAL_META
            causal_meta_set_enabled(0);
#endif
            struct xdb* db = remixdb_open(kDbDir, 256, (size_t)cfg.memtable_mb, true);
            if (!db) logger << "错误：无法重新打开 RemixDB 进行库外推断。\n";
            else {
                struct xdb_ref* ref = remixdb_ref(db);
                for (const PreparedSample& sample : samples) {
                    logger << "试验品：" << sample.file.file_id << "\n";
                    SampleInferenceResult r = infer_one_sample_from_db(
                            ref,
                            sample,
                            cfg,
                            stats.read_seconds,
                            stats.infer_seconds,
                            stats.merge_seconds,
                            logger);
                    stats.original_windows += r.original_windows;
                    stats.final_segments += (long long)r.segments.size();
                    for (const Segment& seg : r.segments) {
                        logger << seg.start_label << "至" << seg.end_label << " | 推断："
                               << format_graph_ordered(seg.graph, cfg.requested_aliases) << "\n";
                    }
                }
                remixdb_unref(ref);
                remixdb_close(db);
            }
        }
        stats.range_section_seconds = now_seconds() - section_t0;

        double compression = 0.0;
        if (stats.original_windows > 0) {
            compression = (double)(stats.original_windows - stats.final_segments) * 100.0 / (double)stats.original_windows;
            if (compression < 0.0) compression = 0.0;
        }
        logger << "本次范围查询小节：耗时:" << fmt_seconds(stats.range_section_seconds)
               << "s，合并压缩率:" << fmt_percent(compression) << "\n\n";

        stats.total_seconds = now_seconds() - total_t0;
        write_csv_row(stat, {
                normalize_excel_cell(cfg.raw_selector), excel_formula_safe_if_needed(cfg.raw_time_range), cfg.resolution_display,
                aliases_display, std::to_string(cfg.window_length), std::to_string(cfg.tau), std::to_string(cfg.delta),
                fmt_double_compact(cfg.theta), std::to_string(cfg.memtable_mb), yes_no(cfg.use_covmeta_cdit),
                fmt_seconds(stats.total_seconds), fmt_seconds(stats.insert_seconds), fmt_percent(compression),
                fmt_seconds(stats.read_seconds), fmt_seconds(stats.infer_seconds), fmt_seconds(stats.merge_seconds)
        });
        logger.flush();
        clean_db_dir();
        return stats;
    }

} // 匿名命名空间结束

// Fluxnet 入库与库外推断对照实验入口，加载配置并逐组执行实验。
extern "C" int run_outdb_fluxnet_experiments(void) {
    std::filesystem::create_directories(kLogDir);
    Logger logger(kPrintLog);
#if OUTDB_FLUXNET_HAS_CAUSAL_META
    causal_meta_set_enabled(0);
#endif
    std::ofstream stat(kStatCsv, std::ios::binary | std::ios::trunc);
    if (!stat.good()) {
        logger << "错误：无法创建统计日志文件：" << kStatCsv << "\n";
        return 1;
    }
    write_stat_header(stat);
    std::vector<ExperimentConfig> configs = load_experiment_configs(logger);
    int exp_id = 1;
    for (const ExperimentConfig& cfg : configs) run_one_experiment(exp_id++, cfg, stat, logger);
    clean_db_dir();
    logger.flush();
    return 0;
}
