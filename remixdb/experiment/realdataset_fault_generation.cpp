/*
 * 本文件负责根据配置生成真实数据集的故障片段与后续修复数据，并输出相应数据文件。
 */

#include "realdataset_fault_generation.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdint.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

struct IntRange {
    int lo;
    int hi;
};

struct ExperimentConfig {
    std::string dataset;
    std::string resolution;
    std::string ratio_label;
    double ratio;
    IntRange fault_len;
    std::string fault_len_label;
    IntRange repair_delay;
    std::string repair_delay_label;
};

struct FaultEvent {
    size_t start;
    size_t len;
    size_t delay;
};

struct FileGenerationStats {
    bool ok;
    std::string input_path;
    std::string output_path;
    size_t input_rows;
    size_t output_rows;
    size_t events;
    size_t fault_rows;

    FileGenerationStats()
            : ok(false), input_rows(0), output_rows(0), events(0), fault_rows(0)
    {
    }
};

static std::string
trim(const std::string &s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

static void
strip_cr(std::string &s)
{
    if (!s.empty() && s[s.size() - 1] == '\r') {
        s.erase(s.size() - 1);
    }
}

static void
strip_utf8_bom(std::string &s)
{
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

static std::string
remove_ascii_spaces(const std::string &s)
{
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isspace(c)) {
            r.push_back(static_cast<char>(c));
        }
    }
    return r;
}

static bool
ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string
to_lower_ascii(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 'A' && c <= 'Z') {
            s[i] = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

static std::string
path_basename(const std::string &path)
{
    const size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

static std::string
path_dirname(const std::string &path)
{
    const size_t p = path.find_last_of('/');
    return p == std::string::npos ? "." : path.substr(0, p);
}

static std::string
join_path(const std::string &a, const std::string &b)
{
    if (a.empty() || a == ".") {
        return b;
    }
    if (a[a.size() - 1] == '/') {
        return a + b;
    }
    return a + "/" + b;
}

static std::string
csv_stem(const std::string &filename)
{
    if (ends_with(filename, ".csv")) {
        return filename.substr(0, filename.size() - 4);
    }
    return filename;
}

static bool
is_dir_path(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool
is_regular_path(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static void
collect_files_recursive(const std::string &dir, std::vector<std::string> &files)
{
    DIR *dp = opendir(dir.c_str());
    if (!dp) {
        return;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dp)) != NULL) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") {
            continue;
        }

        const std::string path = join_path(dir, name);
        if (is_dir_path(path)) {
            collect_files_recursive(path, files);
        } else if (is_regular_path(path)) {
            files.push_back(path);
        }
    }

    closedir(dp);
}

static bool
is_generated_fault_csv_name(const std::string &name)
{
    return ends_with(name, ".csv") && name.find("_pre_") != std::string::npos;
}

static size_t
remove_generated_fault_csvs(const std::string &root)
{
    std::vector<std::string> files;
    collect_files_recursive(root, files);

    size_t removed = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string name = path_basename(files[i]);
        if (is_generated_fault_csv_name(name)) {
            if (std::remove(files[i].c_str()) == 0) {
                ++removed;
            } else {
                std::cerr << "[WARN] failed to remove generated file: "
                          << files[i] << " (" << std::strerror(errno) << ")\n";
            }
        }
    }
    return removed;
}

static std::vector<std::string>
parse_csv_line(const std::string &line)
{
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

static IntRange
parse_int_range(const std::string &raw)
{
    std::string s = remove_ascii_spaces(trim(raw));
    if (s.size() >= 2 && s[0] == '[' && s[s.size() - 1] == ']') {
        s = s.substr(1, s.size() - 2);
    }

    const size_t comma = s.find(',');
    if (comma == std::string::npos) {
        throw std::runtime_error("invalid range: " + raw);
    }

    const int lo = std::atoi(s.substr(0, comma).c_str());
    const int hi = std::atoi(s.substr(comma + 1).c_str());
    if (lo < 0 || hi < 0 || lo > hi) {
        throw std::runtime_error("invalid range: " + raw);
    }

    IntRange r;
    r.lo = lo;
    r.hi = hi;
    return r;
}

static double
parse_ratio(const std::string &raw)
{
    std::string s = remove_ascii_spaces(trim(raw));
    if (s.empty()) {
        throw std::runtime_error("empty fault ratio");
    }

    double v = 0.0;
    if (s[s.size() - 1] == '%') {
        v = std::atof(s.substr(0, s.size() - 1).c_str()) / 100.0;
    } else {
        v = std::atof(s.c_str());
        if (v > 1.0) {
            v /= 100.0;
        }
    }

    if (v < 0.0) {
        v = 0.0;
    }
    if (v > 1.0) {
        v = 1.0;
    }
    return v;
}

static uint64_t
stable_hash64(const std::string &s)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < s.size(); ++i) {
        h ^= static_cast<unsigned char>(s[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

static int
sample_short_biased_int(int lo, int hi, std::mt19937_64 &rng)
{
    if (lo >= hi) {
        return lo;
    }

    const int span = hi - lo;
    std::geometric_distribution<int> dist(0.22);
    const int x = dist(rng);
    return lo + std::min(x, span);
}

static int
sample_uniform_int(int lo, int hi, std::mt19937_64 &rng)
{
    if (lo >= hi) {
        return lo;
    }
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng);
}

static std::vector<FaultEvent>
generate_fault_events(
        const size_t row_count,
        const double ratio,
        const IntRange &fault_len_range,
        const IntRange &delay_range,
        const uint64_t seed,
        size_t *actual_fault_rows)
{
    std::vector<FaultEvent> events;
    if (actual_fault_rows) {
        *actual_fault_rows = 0;
    }

    if (row_count == 0 || ratio <= 0.0) {
        return events;
    }

    const long long n = static_cast<long long>(row_count);
    int lmin = std::max(1, fault_len_range.lo);
    int lmax = std::max(lmin, fault_len_range.hi);
    int dmin = std::max(0, delay_range.lo);
    int dmax = std::max(dmin, delay_range.hi);

    long long target = static_cast<long long>(std::llround(static_cast<double>(n) * ratio));
    if (target <= 0) {
        return events;
    }
    if (target > n) {
        target = n;
    }

    long long feasible_target = target;
    long long e_min = 0;
    long long e_max = -1;

    while (feasible_target >= lmin) {
        e_min = (feasible_target + lmax - 1) / lmax;
        const long long e_max_by_len = feasible_target / lmin;
        const long long e_max_by_space =
                (dmin == 0) ? e_max_by_len : ((n - feasible_target) / dmin);
        e_max = std::min(e_max_by_len, e_max_by_space);
        if (e_min >= 1 && e_max >= e_min) {
            break;
        }
        --feasible_target;
    }

    if (feasible_target < lmin || e_max < e_min) {
        return events;
    }

    target = feasible_target;

    const long long preferred_mean =
            std::max<long long>(lmin, lmin + std::max(0, lmax - lmin) / 5);
    long long e_pref = (target + preferred_mean - 1) / preferred_mean;
    if (e_pref < e_min) {
        e_pref = e_min;
    }
    if (e_pref > e_max) {
        e_pref = e_max;
    }

    const size_t event_count = static_cast<size_t>(e_pref);
    std::mt19937_64 rng(seed);

    std::vector<int> lengths(event_count, 0);
    long long remaining_faults = target;
    for (size_t i = 0; i < event_count; ++i) {
        const long long rem_events = static_cast<long long>(event_count - i - 1);
        const long long lo =
                std::max<long long>(lmin, remaining_faults - rem_events * lmax);
        const long long hi =
                std::min<long long>(lmax, remaining_faults - rem_events * lmin);
        const int len = sample_short_biased_int(static_cast<int>(lo), static_cast<int>(hi), rng);
        lengths[i] = len;
        remaining_faults -= len;
    }

    std::vector<int> delays(event_count, 0);
    long long used_block_rows = 0;
    long long future_fault_rows = target;
    for (size_t i = 0; i < event_count; ++i) {
        future_fault_rows -= lengths[i];
        const long long rem_events = static_cast<long long>(event_count - i - 1);

        long long max_delay_for_this =
                n - used_block_rows - lengths[i] - future_fault_rows - rem_events * dmin;
        if (max_delay_for_this > dmax) {
            max_delay_for_this = dmax;
        }
        if (max_delay_for_this < dmin) {
            max_delay_for_this = dmin;
        }

        const int delay = sample_uniform_int(dmin, static_cast<int>(max_delay_for_this), rng);
        delays[i] = delay;
        used_block_rows += lengths[i] + delays[i];
    }

    if (used_block_rows > n) {
        return std::vector<FaultEvent>();
    }

    const long long free_rows = n - used_block_rows;
    std::vector<long long> gaps(event_count + 1, 0);
    const long long base_gap = free_rows / static_cast<long long>(event_count + 1);
    const long long rem_gap = free_rows % static_cast<long long>(event_count + 1);

    for (size_t i = 0; i < gaps.size(); ++i) {
        gaps[i] = base_gap;
    }

    std::vector<size_t> gap_ids(gaps.size(), 0);
    for (size_t i = 0; i < gap_ids.size(); ++i) {
        gap_ids[i] = i;
    }
    std::shuffle(gap_ids.begin(), gap_ids.end(), rng);
    for (long long i = 0; i < rem_gap; ++i) {
        gaps[gap_ids[static_cast<size_t>(i)]] += 1;
    }

    long long pos = gaps[0];
    for (size_t i = 0; i < event_count; ++i) {
        FaultEvent ev;
        ev.start = static_cast<size_t>(pos);
        ev.len = static_cast<size_t>(lengths[i]);
        ev.delay = static_cast<size_t>(delays[i]);
        events.push_back(ev);

        pos += lengths[i] + delays[i] + gaps[i + 1];
    }

    if (actual_fault_rows) {
        *actual_fault_rows = static_cast<size_t>(target);
    }
    return events;
}

static std::string
make_fault_row(const std::string &row, const size_t column_count)
{
    if (column_count <= 1) {
        return row;
    }

    const size_t comma = row.find(',');
    if (comma == std::string::npos) {
        return row;
    }

    std::string out = row.substr(0, comma);
    for (size_t i = 1; i < column_count; ++i) {
        out += ",0";
    }
    return out;
}

static std::vector<std::string>
read_all_lines_without_cr(const std::string &path)
{
    std::vector<std::string> lines;
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return lines;
    }

    std::string line;
    while (std::getline(in, line)) {
        strip_cr(line);
        lines.push_back(line);
    }
    return lines;
}

static std::string
make_output_path(const std::string &input_path, const ExperimentConfig &cfg)
{
    const std::string dir = path_dirname(input_path);
    const std::string base = path_basename(input_path);
    const std::string stem = csv_stem(base);
    const std::string name =
            stem + "_" +
            remove_ascii_spaces(cfg.ratio_label) + "_" +
            remove_ascii_spaces(cfg.fault_len_label) + "_" +
            remove_ascii_spaces(cfg.repair_delay_label) + ".csv";
    return join_path(dir, name);
}

static FileGenerationStats
generate_fault_file(const std::string &input_path, const ExperimentConfig &cfg)
{
    FileGenerationStats st;
    st.input_path = input_path;
    st.output_path = make_output_path(input_path, cfg);

    std::vector<std::string> lines = read_all_lines_without_cr(input_path);
    if (lines.empty()) {
        std::cerr << "[WARN] skip empty or unreadable file: " << input_path << "\n";
        return st;
    }

    strip_utf8_bom(lines[0]);
    const std::vector<std::string> header_cols = parse_csv_line(lines[0]);
    const size_t column_count = header_cols.size();

    std::vector<std::string> rows;
    rows.reserve(lines.size() > 0 ? lines.size() - 1 : 0);
    for (size_t i = 1; i < lines.size(); ++i) {
        if (!trim(lines[i]).empty()) {
            rows.push_back(lines[i]);
        }
    }

    st.input_rows = rows.size();

    const uint64_t seed = stable_hash64(
            input_path + "|" +
            cfg.dataset + "|" +
            cfg.resolution + "|" +
            cfg.ratio_label + "|" +
            cfg.fault_len_label + "|" +
            cfg.repair_delay_label);

    size_t actual_fault_rows = 0;
    std::vector<FaultEvent> events = generate_fault_events(
            rows.size(), cfg.ratio, cfg.fault_len, cfg.repair_delay, seed, &actual_fault_rows);

    std::ofstream out(st.output_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[WARN] cannot write output file: " << st.output_path
                  << " (" << std::strerror(errno) << ")\n";
        return st;
    }

    out << lines[0] << "\n";

    size_t i = 0;
    size_t e = 0;
    size_t written_rows = 0;

    while (i < rows.size()) {
        if (e < events.size() && i == events[e].start) {
            const FaultEvent &ev = events[e];

            for (size_t j = 0; j < ev.len && i + j < rows.size(); ++j) {
                out << make_fault_row(rows[i + j], column_count) << "\n";
                ++written_rows;
            }

            for (size_t j = 0; j < ev.delay && i + ev.len + j < rows.size(); ++j) {
                out << rows[i + ev.len + j] << "\n";
                ++written_rows;
            }

            for (size_t j = 0; j < ev.len && i + j < rows.size(); ++j) {
                out << rows[i + j] << "\n";
                ++written_rows;
            }

            i += ev.len + ev.delay;
            ++e;
        } else {
            out << rows[i] << "\n";
            ++written_rows;
            ++i;
        }
    }

    st.ok = true;
    st.events = events.size();
    st.fault_rows = actual_fault_rows;
    st.output_rows = written_rows;
    return st;
}

static bool
is_source_pre_csv_file(const std::string &path)
{
    const std::string name = path_basename(path);
    return ends_with(name, "_pre.csv");
}

static std::vector<std::string>
find_source_files_for_config(const ExperimentConfig &cfg)
{
    std::string root;
    const std::string ds_lower = to_lower_ascii(cfg.dataset);

    if (cfg.dataset.find("河川") != std::string::npos ||
        ds_lower.find("basin") != std::string::npos) {
        root = "./data/basin_data";
    } else if (cfg.dataset.find("生物圈") != std::string::npos ||
               ds_lower.find("fluxnet") != std::string::npos) {
        root = "./data/fluxnet_data";
    } else if (cfg.dataset.find("风洞") != std::string::npos ||
               ds_lower.find("wind") != std::string::npos) {
        root = "./data/windtunnel_data";
    } else {
        std::cerr << "[WARN] unknown dataset name in input config: "
                  << cfg.dataset << "\n";
        return std::vector<std::string>();
    }

    if (!is_dir_path(root)) {
        std::cerr << "[WARN] dataset directory does not exist: " << root << "\n";
        return std::vector<std::string>();
    }

    std::vector<std::string> all;
    collect_files_recursive(root, all);

    std::vector<std::string> selected;
    const std::string res = remove_ascii_spaces(cfg.resolution);
    const bool need_resolution =
            (cfg.dataset.find("生物圈") != std::string::npos ||
             ds_lower.find("fluxnet") != std::string::npos) && !res.empty();

    for (size_t i = 0; i < all.size(); ++i) {
        const std::string name = path_basename(all[i]);
        if (!is_source_pre_csv_file(all[i])) {
            continue;
        }
        // FLUXNET2015 的 ZA-Kru 属于 Tier Two，本项目不为该站点生成故障数据。
        if (need_resolution && all[i].find("ZA-Kru") != std::string::npos) {
            continue;
        }
        if (need_resolution) {
            const std::string tag = "_" + res + "_";
            if (name.find(tag) == std::string::npos) {
                continue;
            }
        }
        selected.push_back(all[i]);
    }

    std::sort(selected.begin(), selected.end());
    return selected;
}

static std::vector<ExperimentConfig>
read_experiment_configs(const std::string &input_csv)
{
    std::vector<ExperimentConfig> configs;
    std::ifstream in(input_csv.c_str(), std::ios::binary);
    if (!in) {
        std::cerr << "[ERROR] cannot open input config file: " << input_csv << "\n";
        return configs;
    }

    std::string line;
    if (!std::getline(in, line)) {
        return configs;
    }
    strip_cr(line);
    strip_utf8_bom(line);

    size_t line_no = 1;
    while (std::getline(in, line)) {
        ++line_no;
        strip_cr(line);
        if (trim(line).empty()) {
            continue;
        }

        std::vector<std::string> fields = parse_csv_line(line);
        while (fields.size() < 5) {
            fields.push_back("");
        }

        try {
            ExperimentConfig cfg;
            cfg.dataset = trim(fields[0]);
            cfg.resolution = trim(fields[1]);
            cfg.ratio_label = remove_ascii_spaces(trim(fields[2]));
            cfg.ratio = parse_ratio(cfg.ratio_label);
            cfg.fault_len_label = remove_ascii_spaces(trim(fields[3]));
            cfg.fault_len = parse_int_range(cfg.fault_len_label);
            cfg.repair_delay_label = remove_ascii_spaces(trim(fields[4]));
            cfg.repair_delay = parse_int_range(cfg.repair_delay_label);
            configs.push_back(cfg);
        } catch (const std::exception &ex) {
            std::cerr << "[WARN] skip invalid config line " << line_no
                      << ": " << ex.what() << "\n";
        }
    }

    return configs;
}

// 真实数据故障生成入口：清理旧生成文件，按配置批量生成故障数据并汇总生成统计。
extern "C" int
run_realdataset_fault_generation_experiments(void)
{
    const std::string input_csv = "./experiment/realdataset_fault_generation_input.csv";

    std::cout << "========== Real Dataset Fault Generation ==========\n";
    std::cout << "Input config: " << input_csv << "\n";

    const size_t removed_basin = remove_generated_fault_csvs("./data/basin_data");
    const size_t removed_fluxnet = remove_generated_fault_csvs("./data/fluxnet_data");
    const size_t removed_windtunnel = remove_generated_fault_csvs("./data/windtunnel_data");

    std::cout << "Removed generated files: basin=" << removed_basin
              << ", fluxnet=" << removed_fluxnet
              << ", windtunnel=" << removed_windtunnel << "\n";

    std::vector<ExperimentConfig> configs = read_experiment_configs(input_csv);
    if (configs.empty()) {
        std::cerr << "[ERROR] no valid experiment config was loaded.\n";
        return 1;
    }

    size_t total_generated_files = 0;
    size_t total_input_rows = 0;
    size_t total_output_rows = 0;
    size_t total_fault_rows = 0;
    size_t total_events = 0;

    for (size_t c = 0; c < configs.size(); ++c) {
        const ExperimentConfig &cfg = configs[c];
        std::vector<std::string> sources = find_source_files_for_config(cfg);

        std::cout << "\n[CONFIG " << (c + 1) << "/" << configs.size() << "] "
                  << "dataset=" << cfg.dataset
                  << ", resolution=" << (cfg.resolution.empty() ? "-" : cfg.resolution)
                  << ", fault_ratio=" << cfg.ratio_label
                  << ", fault_len=" << cfg.fault_len_label
                  << ", repair_delay=" << cfg.repair_delay_label
                  << ", source_files=" << sources.size() << "\n";

        size_t cfg_generated = 0;
        size_t cfg_input_rows = 0;
        size_t cfg_output_rows = 0;
        size_t cfg_fault_rows = 0;
        size_t cfg_events = 0;

        const size_t preview_limit = 5;

        for (size_t i = 0; i < sources.size(); ++i) {
            FileGenerationStats st = generate_fault_file(sources[i], cfg);
            if (!st.ok) {
                continue;
            }

            ++cfg_generated;
            cfg_input_rows += st.input_rows;
            cfg_output_rows += st.output_rows;
            cfg_fault_rows += st.fault_rows;
            cfg_events += st.events;

            if (i < preview_limit) {
                std::cout << "  generated: " << st.output_path
                          << " | rows " << st.input_rows << " -> " << st.output_rows
                          << ", fault_rows=" << st.fault_rows
                          << ", events=" << st.events << "\n";
            }
        }

        if (sources.size() > preview_limit) {
            std::cout << "  ... " << (sources.size() - preview_limit)
                      << " more files generated for this config.\n";
        }

        const double actual_ratio =
                cfg_input_rows == 0 ? 0.0 :
                (100.0 * static_cast<double>(cfg_fault_rows) / static_cast<double>(cfg_input_rows));

        std::cout << "  summary: generated_files=" << cfg_generated
                  << ", input_rows=" << cfg_input_rows
                  << ", output_rows=" << cfg_output_rows
                  << ", fault_rows=" << cfg_fault_rows
                  << ", actual_fault_ratio=" << actual_ratio << "%"
                  << ", events=" << cfg_events << "\n";

        total_generated_files += cfg_generated;
        total_input_rows += cfg_input_rows;
        total_output_rows += cfg_output_rows;
        total_fault_rows += cfg_fault_rows;
        total_events += cfg_events;
    }

    const double total_ratio =
            total_input_rows == 0 ? 0.0 :
            (100.0 * static_cast<double>(total_fault_rows) / static_cast<double>(total_input_rows));

    std::cout << "\n========== Fault Generation Finished ==========\n";
    std::cout << "Total generated files: " << total_generated_files << "\n";
    std::cout << "Total input rows: " << total_input_rows << "\n";
    std::cout << "Total output rows: " << total_output_rows << "\n";
    std::cout << "Total fault rows: " << total_fault_rows
              << " (" << total_ratio << "%)\n";
    std::cout << "Total fault events: " << total_events << "\n";

    return 0;
}
