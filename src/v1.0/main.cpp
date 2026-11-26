#include <algorithm>
#include <chrono>
#include <cctype>
#include <csignal>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

volatile std::sig_atomic_t keep_running = 1;

struct Process {
    int pid;
    std::string name;
    long rss_kib;
    unsigned long long cpu_ticks;
    double cpu_percent;
};

void stop_monitor(int) {
    keep_running = 0;
}

bool is_pid(const char* name) {
    return *name != '\0' &&
           std::all_of(name, name + std::char_traits<char>::length(name),
                       [](unsigned char ch) { return std::isdigit(ch); });
}

unsigned long long read_total_cpu_ticks() {
    std::ifstream stat("/proc/stat");
    std::string label;
    unsigned long long value;
    unsigned long long total = 0;

    stat >> label;
    if (label != "cpu") {
        return 0;
    }

    while (stat >> value) {
        total += value;
    }
    return total;
}

bool read_process(int pid, Process& process) {
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    process.pid = pid;
    process.rss_kib = 0;

    while (std::getline(status, line)) {
        if (line.rfind("Name:", 0) == 0) {
            process.name = line.substr(5);
            process.name.erase(0, process.name.find_first_not_of(" \t"));
        } else if (line.rfind("VmRSS:", 0) == 0) {
            try {
                process.rss_kib = std::stol(line.substr(6));
            } catch (const std::exception&) {
                return false;
            }
        }
    }

    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string stat_line;
    if (!std::getline(stat, stat_line)) {
        return false;
    }

    const std::size_t command_end = stat_line.rfind(')');
    if (command_end == std::string::npos) {
        return false;
    }

    std::istringstream fields(stat_line.substr(command_end + 2));
    std::string field;
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    for (int position = 0; fields >> field; ++position) {
        if (position == 11) {
            utime = std::stoull(field);
        } else if (position == 12) {
            stime = std::stoull(field);
            break;
        }
    }

    process.cpu_ticks = utime + stime;
    return true;
}

std::vector<Process> read_processes(
    const std::unordered_map<int, unsigned long long>& previous_ticks,
    unsigned long long total_tick_delta) {
    DIR* proc = opendir("/proc");
    if (proc == nullptr) {
        return {};
    }

    std::vector<Process> processes;
    while (dirent* entry = readdir(proc)) {
        if (!is_pid(entry->d_name)) {
            continue;
        }

        Process process;
        if (!read_process(std::stoi(entry->d_name), process)) {
            continue;
        }

        const auto previous = previous_ticks.find(process.pid);
        process.cpu_percent =
            previous != previous_ticks.end() && total_tick_delta != 0
                ? 100.0 * (process.cpu_ticks - previous->second) / total_tick_delta
                : 0.0;
        processes.push_back(std::move(process));
    }
    closedir(proc);
    return processes;
}

}  // namespace

int main(int argc, char* argv[]) {
    const unsigned int interval_seconds =
        argc == 2 ? static_cast<unsigned int>(std::stoul(argv[1])) : 5;
    if (interval_seconds == 0) {
        std::cerr << "Interval must be at least one second.\n";
        return 1;
    }

    std::signal(SIGINT, stop_monitor);
    std::unordered_map<int, unsigned long long> previous_ticks;
    unsigned long long previous_total_ticks = read_total_cpu_ticks();

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));

        const unsigned long long total_ticks = read_total_cpu_ticks();
        const unsigned long long total_tick_delta = total_ticks - previous_total_ticks;
        std::vector<Process> processes =
            read_processes(previous_ticks, total_tick_delta);

        previous_ticks.clear();
        for (const Process& process : processes) {
            previous_ticks[process.pid] = process.cpu_ticks;
        }
        previous_total_ticks = total_ticks;

        std::sort(processes.begin(), processes.end(),
                  [](const Process& left, const Process& right) {
                      return left.rss_kib > right.rss_kib;
                  });

        std::cout << "\nTop processes by resident memory (sampled over "
                  << interval_seconds << " s)\n"
                  << std::left << std::setw(8) << "PID" << std::setw(24)
                  << "NAME" << std::right << std::setw(12) << "RSS MiB"
                  << std::setw(10) << "CPU %" << '\n';

        const std::size_t result_count = std::min<std::size_t>(10, processes.size());
        for (std::size_t index = 0; index < result_count; ++index) {
            const Process& process = processes[index];
            std::cout << std::left << std::setw(8) << process.pid
                      << std::setw(24) << process.name << std::right
                      << std::fixed << std::setprecision(1) << std::setw(12)
                      << process.rss_kib / 1024.0 << std::setw(10)
                      << process.cpu_percent << '\n';
        }
        std::cout.flush();
    }
}
