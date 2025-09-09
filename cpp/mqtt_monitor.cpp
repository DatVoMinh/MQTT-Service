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

// Timeouts / behavior
static const double NO_MESSAGE_TIMEOUT = 20.0;   // seconds
static const double RESTART_WAIT_TIME = 60.0;   // seconds
static const int MAX_CONSECUTIVE_KILLS = 10;

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

class TerminalLogger {
public:
    TerminalLogger() = default;

    void info(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mu_);
        std::cout << msg << std::endl;
    }

private:
    std::mutex mu_;
};

// Kill helpers
static void kill_robot_adapter(TerminalLogger& logger,
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
            long pid = strtol(buf, nullptr, 10);
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
}

// MQTT callback
class MonitorCallback : public virtual mqtt::callback {
public:
    MonitorCallback(mqtt::async_client& client, 
                    std::atomic<long long>& last_msg_ms,
                    std::atomic<int>& consecutive_kill_count,
                    std::atomic<bool>& connected,
                    TerminalLogger& logger)
        : client_(client), last_msg_ms_(last_msg_ms), consecutive_kill_count_(consecutive_kill_count), 
          connected_(connected), logger_(logger) {}

    void connected(const std::string& cause) override {
        connected_.store(true);
        // Reset last message time to give adapter time to send messages
        last_msg_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        std::ostringstream oss;
        oss << "[" << now_time_str() << "] Connected: " << cause;
        logger_.info(oss.str());

        try {
            client_.subscribe(SUB_TOPIC, 1); // QoS=1 for reliable delivery
            oss.str("");
            oss << "[" << now_time_str() << "] Subscribed to " << SUB_TOPIC;
            logger_.info(oss.str());
        } catch (const std::exception& e) {
            oss.str("");
            oss << "[" << now_time_str() << "] Subscribe failed: " << e.what();
            logger_.info(oss.str());
        }
    }

    void connection_lost(const std::string& cause) override {
        connected_.store(false);
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
            std::ostringstream oss;
            oss << "[" << now_time_str() << "] Reset consecutive kill count to 0.";
            logger_.info(oss.str());
        }
    }

    void delivery_complete(mqtt::delivery_token_ptr) override {}

private:
    mqtt::async_client& client_;
    std::atomic<long long>& last_msg_ms_;
    std::atomic<int>& consecutive_kill_count_;
    std::atomic<bool>& connected_;
    TerminalLogger& logger_;
};

int main() {
    TerminalLogger logger;

    // Handle SIGINT/SIGTERM
    std::signal(SIGINT, [](int){ g_running.store(false); });
    std::signal(SIGTERM, [](int){ g_running.store(false); });

    std::atomic<long long> last_msg_ms{ 
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() 
    };
    std::atomic<int> consecutive_kill_count{0};
    std::atomic<bool> connected_{false};
    std::atomic<long long> last_kill_time{0};   // milliseconds since epoch

    const std::string server_uri = PROTOCOL + "://" + BROKER_HOST + ":" + std::to_string(BROKER_PORT) + BASE_PATH;
    const std::string client_id = std::string("mqtt-monitor-") + std::to_string(::getpid());

    mqtt::async_client client(server_uri, client_id);

    MonitorCallback cb(client, last_msg_ms, consecutive_kill_count, connected_, logger);
    client.set_callback(cb);

    mqtt::connect_options connOpts;
    connOpts.set_keep_alive_interval(30);
    connOpts.set_mqtt_version(4); // MQTT v3.1.1
    connOpts.set_clean_session(true);
    connOpts.set_automatic_reconnect(true); // Enable automatic reconnect

    try {
        client.connect(connOpts)->wait();
        
        std::ostringstream oss;
        oss << "[" << now_time_str() << "] MQTT Monitor started.\n"
            << "  - Broker: " << server_uri << "\n"
            << "  - Topic: " << SUB_TOPIC << "\n"
            << "  - Timeout: " << NO_MESSAGE_TIMEOUT << "s\n"
            << "  - Kill cooldown base: " << RESTART_WAIT_TIME << "s\n"
            << "  - Max consecutive kills: " << MAX_CONSECUTIVE_KILLS;
        logger.info(oss.str());
    } catch (const std::exception& e) {
        std::ostringstream oss;
        oss << "[" << now_time_str() << "] MQTT connect failed: " << e.what();
        logger.info(oss.str());
        return 1;
    }

    // Watchdog thread
    std::thread watchdog([&](){
        try {
            while (g_running.load()) {
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                
                if (connected_.load()) {
                    const auto last_msg = last_msg_ms.load();
                    const double delta_s = (now_ms - last_msg) / 1000.0;

                    if (delta_s >= NO_MESSAGE_TIMEOUT) {
                        const auto last_kill = last_kill_time.load();
                        const double time_since_kill_s = (now_ms - last_kill) / 1000.0;

                        // New: cooldown grows with consecutive_kill_count (always >= base)
                        const int kill_count = std::max(1, consecutive_kill_count.load());
                        const double cooldown_needed_s = RESTART_WAIT_TIME * static_cast<double>(kill_count);

                        if (time_since_kill_s >= cooldown_needed_s) {
                            std::ostringstream oss;
                            oss << "[" << now_time_str() << "] No MQTT messages for "
                                << std::fixed << std::setprecision(1) << delta_s << "s (>="
                                << NO_MESSAGE_TIMEOUT << "s). Initiating kill. "
                                << "(cooldown_needed=" << cooldown_needed_s << "s, consecutive_kills=" << consecutive_kill_count.load() << ")";
                            logger.info(oss.str());

                            kill_robot_adapter(logger, consecutive_kill_count);
                            last_kill_time.store(now_ms);
                        } else {
                            std::ostringstream oss;
                            oss << "[" << now_time_str() << "] In cooldown period. "
                                << std::fixed << std::setprecision(1) << time_since_kill_s
                                << "s since last kill. Need "
                                << (cooldown_needed_s - time_since_kill_s)
                                << "s until next kill allowed "
                                << "(cooldown_needed=" << cooldown_needed_s
                                << "s, consecutive_kills=" << consecutive_kill_count.load() << ").";
                            logger.info(oss.str());
                        }
                    }
                }
                std::this_thread::sleep_for(1000ms);
            }
        } catch (const std::exception& e) {
            std::ostringstream oss;
            oss << "[" << now_time_str() << "] Watchdog thread exception: " << e.what();
            logger.info(oss.str());
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
        client.disconnect()->wait();
    } catch (...) {}

    if (watchdog.joinable()) watchdog.join();

    logger.info("[" + now_time_str() + "] MQTT Monitor stopped.");
    return 0;
}
