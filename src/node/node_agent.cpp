#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <atomic>
#include <ctime>

std::string node_id;
int task_listener_fd = -1;
int manager_fd = -1;
std::atomic<bool> running{true};
std::mutex send_mutex;  

void log(const std::string &level, const std::string &msg) {
    auto t = std::time(nullptr);
    char buf[100];
    std::strftime(buf, sizeof(buf), "[%F %T]", std::localtime(&t));
    std::cout << buf << " [" << level << "]    " << msg << std::endl;
}

void signal_handler(int signum) {
    log("INFO", "Caught signal " + std::to_string(signum) + ". Shutting down node...");
    running = false;

    if (manager_fd != -1) close(manager_fd);
    if (task_listener_fd != -1) close(task_listener_fd);
    exit(0);
}

void send_to_manager(const std::string& msg) {
    std::lock_guard<std::mutex> lock(send_mutex);
    if (manager_fd != -1) {
        ssize_t sent = send(manager_fd, msg.c_str(), msg.size(), 0);
        if (sent < 0) {
            log("ERROR", "Failed to send message to manager: " + msg);
        }
    }
}

void execute_task(const std::string &task) {
    log("INFO", "Node " + node_id + ": Received task: " + task);
    std::this_thread::sleep_for(std::chrono::seconds(1));  
    log("INFO", "Node " + node_id + ": Completed task: " + task);

    std::string clean_task = task;
    size_t first = clean_task.find_first_not_of(" \t\n\r");
    size_t last = clean_task.find_last_not_of(" \t\n\r");
    clean_task = (first != std::string::npos) ? clean_task.substr(first, last - first + 1) : "";

    if (!clean_task.empty()) {
        std::string done_msg = "TASK_DONE " + clean_task + "\n";
        send_to_manager(done_msg);
    }
}

namespace NodeAgentConfig {
    std::string MANAGER_DEFAULT_IP = "127.0.0.1";
    int MANAGER_DEFAULT_PORT = 5000;
    int NODE_DEFAULT_LISTEN_PORT = 6000; 
    int CPU_MONITOR_INTERVAL_SEC = 1; 
}

void task_listener(int port) {
    sockaddr_in server_addr{}, client_addr{};
    socklen_t client_len = sizeof(client_addr);
    task_listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (task_listener_fd < 0) {
        log("ERROR", "Node " + node_id + ": Failed to create task listener socket.");
        return;
    }

    int opt = 1;
    setsockopt(task_listener_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(task_listener_fd, (sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        log("ERROR", "Node " + node_id + ": Bind failed on task port.");
        return;
    }

    listen(task_listener_fd, 5);
    log("INFO", "Node " + node_id + ": Listening for tasks on port " + std::to_string(port) + "...");

    while (running) {
        int client_fd = accept(task_listener_fd, (sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        char buffer[1024] = {0};
        ssize_t valread = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (valread <= 0) {
            close(client_fd);
            continue;
        }
        buffer[valread] = '\0';
        std::string task_raw(buffer);

        size_t first = task_raw.find_first_not_of(" \t\n\r");
        size_t last = task_raw.find_last_not_of(" \t\n\r");
        task_raw = (first == std::string::npos) ? "" : task_raw.substr(first, last - first + 1);

        if (task_raw == "PING") {
            log("INFO", "Node " + node_id + ": Received PING from manager.");
            const char* pong_msg = "PONG\n";
            send(client_fd, pong_msg, strlen(pong_msg), 0);
            close(client_fd); 
            continue; 
        } else if (task_raw == "SHUTDOWN") {
            log("INFO", "Node " + node_id + ": Received shutdown signal from manager.");
            running = false;
            close(client_fd);
            break;
        } else if (!task_raw.empty()) {
            execute_task(task_raw);
        }

        close(client_fd);
    }
}

float calculate_cpu_usage() {
    static long long prev_total = 0, prev_idle = 0;
    std::ifstream stat_file("/proc/stat");
    std::string line;
    std::getline(stat_file, line);
    std::istringstream iss(line);
    std::string cpu;
    long long user, nice, system, idle, iowait, irq, softirq, steal;
    iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
    long long idle_time = idle + iowait;
    long long non_idle = user + nice + system + irq + softirq + steal;
    long long total = idle_time + non_idle;
    long long total_diff = total - prev_total;
    long long idle_diff = idle_time - prev_idle;
    prev_total = total;
    prev_idle = idle_time;
    return (total_diff == 0) ? 0.0f : (100.0f * (total_diff - idle_diff) / total_diff);
}

namespace NodeMetrics {

    float calculate_memory_usage() {
        std::ifstream mem_file("/proc/meminfo");
        std::string line;
        long total_mem_kb = 0;
        long free_mem_kb = 0;
        long buffers_kb = 0;
        long cached_kb = 0;
        long s_reclaimable_kb = 0; 

        while (std::getline(mem_file, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::istringstream iss(line);
                std::string key;
                iss >> key >> total_mem_kb;
            } else if (line.rfind("MemFree:", 0) == 0) {
                std::istringstream iss(line);
                std::string key;
                iss >> key >> free_mem_kb;
            } else if (line.rfind("Buffers:", 0) == 0) {
                std::istringstream iss(line);
                std::string key;
                iss >> key >> buffers_kb;
            } else if (line.rfind("Cached:", 0) == 0) {
                std::istringstream iss(line);
                std::string key;
                iss >> key >> cached_kb;
            } else if (line.rfind("SReclaimable:", 0) == 0) { 
                 std::istringstream iss(line);
                 std::string key;
                 iss >> key >> s_reclaimable_kb;
            }
        }

        if (total_mem_kb == 0) return 0.0f;

        long used_mem_kb = total_mem_kb - (free_mem_kb + buffers_kb + cached_kb + s_reclaimable_kb);
        if (used_mem_kb < 0) used_mem_kb = 0; 

        return (static_cast<float>(used_mem_kb) / total_mem_kb) * 100.0f;
    }


    void enhanced_monitor() {
        while (running) {
            float cpu_usage = calculate_cpu_usage(); 
            float mem_usage = calculate_memory_usage(); 

            std::ostringstream oss;
            oss << "CPU_USAGE " << cpu_usage << "\n";
            oss << "MEM_USAGE " << mem_usage << "\n"; 
            send_to_manager(oss.str());

            std::this_thread::sleep_for(std::chrono::seconds(NodeAgentConfig::CPU_MONITOR_INTERVAL_SEC));
        }
    }
} 


int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " <node_id> <manager_ip> <manager_port> <listen_port>\n";
        return 1;
    }

    signal(SIGINT, signal_handler);

    node_id = argv[1];
    std::string manager_ip = argv[2];
    int manager_port = std::stoi(argv[3]);
    int task_port = std::stoi(argv[4]);

    log("INFO", "NodeAgent " + node_id + ": initialized.");

    sockaddr_in manager_addr{};
    manager_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (manager_fd < 0) {
        log("ERROR", "Node " + node_id + ": Failed to create socket to manager.");
        return 1;
    }

    manager_addr.sin_family = AF_INET;
    manager_addr.sin_port = htons(manager_port);
    inet_pton(AF_INET, manager_ip.c_str(), &manager_addr.sin_addr);

    if (connect(manager_fd, (sockaddr *)&manager_addr, sizeof(manager_addr)) < 0) {
        log("ERROR", "Node " + node_id + ": Could not connect to manager.");
        close(manager_fd);
        return 1;
    }

    log("INFO", "Node " + node_id + ": Connected to manager at " + manager_ip + ":" + std::to_string(manager_port));

    std::string reg_msg = "REGISTER " + node_id + " " + std::to_string(task_port) + "\n";
    send_to_manager(reg_msg);
    log("INFO", "Node " + node_id + ": Sent registration message to manager.");

    std::thread monitor(NodeMetrics::enhanced_monitor);
    std::thread listener(task_listener, task_port);

    listener.join();
    running = false; 
    monitor.join();

    if (manager_fd != -1) close(manager_fd);
    if (task_listener_fd != -1) close(task_listener_fd);

    log("INFO", "Node " + node_id + ": Shutdown complete.");
    return 0;
}