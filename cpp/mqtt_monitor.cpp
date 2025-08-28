// mqtt_monitor.cpp
// Timeout-only MQTT monitor using Eclipse Paho MQTT C++ (headers under /usr/include/mqtt)

#include <mqtt/async_client.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <algorithm>

using namespace std::chrono_literals;

// ---------- Config ----------
static const std::string BROKER_HOST = "127.0.0.1";
static const int BROKER_PORT = 9001;            // WebSockets
static const std::string BASE_PATH = "/mqtt";   // WS path
static const std::string PROTOCOL = "ws";       // "ws" or "wss"

static const std::string SUB_TOPIC = "fleetmanagement/3/robot/info";

// Logging
static const size_t LOG_FILE_MAX_BYTES = 1 * 1024 * 1024;     // 1 MB
static const size_t LOG_TOTAL_CAP_BYTES = 50 * 1024 * 1024;   // 50 MB
static const std::string LOG_FILE_PREFIX = "mqtt_monitor_timeout_kill_adapter_";
// static const std::string AUTO_KILL_LOG_DIR = "/home/wcs2/AMR/auto_start/logs"; // base dir; logs in <dir>/log_mqtt_monitor
static const std::string AUTO_KILL_LOG_DIR = "/home/dat/denso_ws/FAKER"; // base dir; logs in <dir>/log_mqtt_monitor

// Timeouts / behavior
static const double NO_MESSAGE_TIMEOUT = 20.0;   // seconds
static const double RESTART_WAIT_TIME = 120.0;   // seconds
static const int MAX_CONSECUTIVE_KILLS = 3;

// ---------- Utilities ----------
static std::atomic<bool> g_running{true};

static std::string now_time_str() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%d_%b_%Y_%H_%M_%S");
    return oss.str();
}

class RotatingLogger {
public:
    explicit RotatingLogger(const std::string& base_dir,
                             const std::string& prefix,
                             size_t max_bytes,
                             size_t total_cap_bytes)
        : base_dir_(base_dir), prefix_(prefix), max_bytes_(max_bytes), total_cap_bytes_(total_cap_bytes) {
        try {
            std::filesystem::create_directories(base_dir_);
        } catch (...) {}
        open_new_file();
    }

    void info(const std::string& msg) {
        std::lock_guard<std::mutex> lk(mu_);
        try_rotate();
        try {
            if (stream_.is_open()) {
                stream_ << msg << '\n';
                stream_.flush();
            }
        } catch (...) {}
        // Mirror to stdout as well
        std::cout << msg << std::endl;
    }

private:
    std::mutex mu_;
    std::string base_dir_;
    std::string prefix_;
    size_t max_bytes_;
    size_t total_cap_bytes_;
    std::ofstream stream_;
    std::string current_path_;

    std::string timestamp_name() {
        return base_dir_ + "/" + prefix_ + now_time_str() + ".log";
    }

    void open_new_file() {
        current_path_ = timestamp_name();
        try { stream_.close(); } catch (...) {}
        stream_.open(current_path_, std::ios::app);
    }

    bool should_rotate() {
        try {
            return std::filesystem::file_size(current_path_) >= max_bytes_;
        } catch (...) {
            return false;
        }
    }

    void cleanup_total_size() {
        try {
            using file_time_t = std::filesystem::file_time_type;
            std::vector<std::pair<std::filesystem::path, file_time_t>> files;
            for (auto& p : std::filesystem::directory_iterator(base_dir_)) {
                if (!p.is_regular_file()) continue;
                auto name = p.path().filename().string();
                if (name.rfind(prefix_, 0) != 0) continue; // must start with prefix
                auto t = std::filesystem::last_write_time(p.path());
                files.emplace_back(p.path(), t);
            }
            std::sort(files.begin(), files.end(), [](const auto& a, const auto& b){ return a.second < b.second; });

            size_t total = 0;
            for (auto& f : files) {
                try { total += std::filesystem::file_size(f.first); } catch (...) {}
            }

            size_t idx = 0;
            while (total > total_cap_bytes_ && idx < files.size()) {
                auto p = files[idx].first;
                size_t sz = 0;
                try { sz = std::filesystem::file_size(p); } catch (...) {}
                try { std::filesystem::remove(p); } catch (...) {}
                if (total >= sz) total -= sz; else total = 0;
                idx++;
            }
        } catch (...) {}
    }

    void try_rotate() {
        if (should_rotate()) {
            try { stream_.close(); } catch (...) {}
            open_new_file();
            cleanup_total_size();
        }
    }
};

static std::string resolve_log_dir() {
    std::string base = AUTO_KILL_LOG_DIR.empty() ? std::filesystem::current_path().string() : AUTO_KILL_LOG_DIR;
    return base + "/log_mqtt_monitor";
}

// Kill helpers
static void kill_robot_adapter(RotatingLogger& logger,
                               std::atomic<int>& consecutive_kill_count) {
    if (consecutive_kill_count.load() >= MAX_CONSECUTIVE_KILLS) {
        std::ostringstream oss;
        oss << "[" << now_time_str() << "] Kill suppressed: reached max consecutive kills (" << MAX_CONSECUTIVE_KILLS << ").";
        logger.info(oss.str());
        return;
    }

    {
        std::ostringstream oss;
        oss << "[" << now_time_str() << "] Killing robot_adapter processes (single pass)...";
        logger.info(oss.str());
    }

    // Use pgrep -f robot_adapter and kill -9
    FILE* fp = popen("pgrep -f robot_adapter", "r");
    if (fp) {
        char buf[256];
        while (fgets(buf, sizeof(buf), fp)) {
            int pid = std::atoi(buf);
            if (pid > 0) {
                if (kill(pid, SIGKILL) == 0) {
                    std::ostringstream oss;
                    oss << "[" << now_time_str() << "] Killed PID " << pid;
                    logger.info(oss.str());
                } else {
                    std::ostringstream oss;
                    oss << "[" << now_time_str() << "] Failed to kill PID " << pid << ": " << std::strerror(errno);
                    logger.info(oss.str());
                }
            }
        }
        pclose(fp);
    } else {
        std::ostringstream oss;
        oss << "[" << now_time_str() << "] Unable to run pgrep.";
        logger.info(oss.str());
    }

    consecutive_kill_count.fetch_add(1);
    {
        std::ostringstream oss;
        oss << "[" << now_time_str() << "] Consecutive kills so far: "
            << consecutive_kill_count.load() << "/" << MAX_CONSECUTIVE_KILLS;
        logger.info(oss.str());
    }

    {
        std::ostringstream oss;
        oss << "[" << now_time_str() << "] Waiting " << RESTART_WAIT_TIME << "s before resuming evaluation...";
        logger.info(oss.str());
    }
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() < RESTART_WAIT_TIME) {
        std::this_thread::sleep_for(500ms);
    }
    {
        std::ostringstream oss;
        oss << "[" << now_time_str() << "] Resume monitoring by message evaluation.";
        logger.info(oss.str());
    }
}

// MQTT callback
class MonitorCallback : public virtual mqtt::callback {
public:
    MonitorCallback(std::atomic<long long>& last_msg_ms,
                    std::atomic<int>& consecutive_kill_count,
                    RotatingLogger& logger)
        : last_msg_ms_(last_msg_ms), consecutive_kill_count_(consecutive_kill_count), logger_(logger) {}

    void connected(const std::string& cause) override {
        std::ostringstream oss;
        oss << "[" << now_time_str() << "] Connected: " << cause;
        logger_.info(oss.str());
    }

    void connection_lost(const std::string& cause) override {
        std::ostringstream oss;
        oss << "[" << now_time_str() << "] Connection lost: " << cause;
        logger_.info(oss.str());
    }

    void message_arrived(mqtt::const_message_ptr msg) override {
        last_msg_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        // Reset consecutive kill counter on any message
        if (consecutive_kill_count_.load() != 0) {
            consecutive_kill_count_.store(0);
        }
    }

    void delivery_complete(mqtt::delivery_token_ptr) override {}

private:
    std::atomic<long long>& last_msg_ms_;
    std::atomic<int>& consecutive_kill_count_;
    RotatingLogger& logger_;
};

int main() {
    // Prepare log directory
    const std::string log_dir = resolve_log_dir();
    RotatingLogger logger(log_dir, LOG_FILE_PREFIX, LOG_FILE_MAX_BYTES, LOG_TOTAL_CAP_BYTES);

    // Handle SIGINT/SIGTERM
    std::signal(SIGINT, [](int){ g_running.store(false); });
    std::signal(SIGTERM, [](int){ g_running.store(false); });

    std::atomic<long long> last_msg_ms{ std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() };
    std::atomic<int> consecutive_kill_count{0};

    const std::string server_uri = PROTOCOL + "://" + BROKER_HOST + ":" + std::to_string(BROKER_PORT) + BASE_PATH;
    const std::string client_id = std::string("rtamrtf-monitor-") + std::to_string(::getpid());

    mqtt::async_client client(server_uri, client_id);

    MonitorCallback cb(last_msg_ms, consecutive_kill_count, logger);
    client.set_callback(cb);

    mqtt::connect_options connOpts;
    connOpts.set_keep_alive_interval(30);
    connOpts.set_mqtt_version(4); // MQTT v3.1.1 to match broker
    connOpts.set_clean_session(true);

    try {
        client.connect(connOpts)->wait();
        client.start_consuming();
        client.subscribe(SUB_TOPIC, 0)->wait();
        {
            std::ostringstream oss;
            oss << "[" << now_time_str() << "] MQTT Monitor started.\n  - Backend: paho-mqtt C++\n  - Subscribed: " << SUB_TOPIC
                << "\n  - Kill if: no messages for " << NO_MESSAGE_TIMEOUT << "s\n  - Restart wait: " << RESTART_WAIT_TIME
                << "s\n  - Max consecutive kills: " << MAX_CONSECUTIVE_KILLS;
            logger.info(oss.str());
        }
    } catch (const std::exception& e) {
        std::ostringstream oss;
        oss << "[" << now_time_str() << "] MQTT connect failed: " << e.what();
        logger.info(oss.str());
        return 1;
    }

    // Watchdog thread
    std::thread watchdog([&](){
        while (g_running.load()) {
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const auto last_ms = last_msg_ms.load();
            const double delta_s = (now_ms - last_ms) / 1000.0;
            if (delta_s >= NO_MESSAGE_TIMEOUT) {
                {
                    std::ostringstream oss;
                    oss << "[" << now_time_str() << "] No MQTT messages for " << NO_MESSAGE_TIMEOUT << "s. Initiating kill.";
                    logger.info(oss.str());
                }
                kill_robot_adapter(logger, consecutive_kill_count);
                // Reset last message time to avoid immediate re-trigger
                last_msg_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            }
            std::this_thread::sleep_for(100ms);
        }
    });

    // Main loop
    while (g_running.load()) {
        std::this_thread::sleep_for(1s);
    }

    // Cleanup
    try {
        client.unsubscribe(SUB_TOPIC)->wait();
    } catch (...) {}
    try {
        client.stop_consuming();
    } catch (...) {}
    try {
        client.disconnect()->wait();
    } catch (...) {}

    if (watchdog.joinable()) watchdog.join();

    return 0;
}


