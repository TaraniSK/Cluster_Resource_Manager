#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h> 
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <queue>
#include <vector>
#include <atomic>
#include <ctime>
#include <chrono>
#include <set>

using namespace std::chrono;

std::mutex node_mutex;
std::mutex task_mutex;
std::atomic<bool> running{true};

struct NodeInfo {
    std::string id;
    std::string ip;
    int port;
    int sockfd; // This sockfd is for the registration connection, not for task/heartbeat.
                // Heartbeats and task assignments open new sockets.
    bool available = true;
    double cpu_usage = 100.0;
    double mem_usage = 100.0;
    time_point<steady_clock> last_cpu_update = steady_clock::now();
    time_point<steady_clock> last_mem_update = steady_clock::now();
    time_point<steady_clock> last_heartbeat_pong = steady_clock::now();
};

enum class TaskStatus { QUEUED, ASSIGNED, COMPLETED };

struct TaskEntry {
    std::string task;
    TaskStatus status;
    std::string assigned_node;
    int requeued_count = 0; 
};

std::map<std::string, NodeInfo> nodes;
std::queue<std::string> task_queue;
std::map<std::string, TaskEntry> tasks;

class TeeBuf : public std::streambuf {
    std::streambuf* sb1;
    std::streambuf* sb2;

public:
    TeeBuf(std::streambuf* buf1, std::streambuf* buf2) : sb1(buf1), sb2(buf2) {}

protected:
    int overflow(int c) override {
        if (c == EOF) return !EOF;
        if (sb1->sputc(c) == EOF) return EOF;
        if (sb2->sputc(c) == EOF) return EOF;
        return c;
    }

    int sync() override {
        int r1 = sb1->pubsync();
        int r2 = sb2->pubsync();
        return (r1 == 0 && r2 == 0) ? 0 : -1;
    }
};

void log(const std::string &level, const std::string &msg) {
    auto t = std::time(nullptr);
    char buf[100];
    std::strftime(buf, sizeof(buf), "[%F %T]", std::localtime(&t));
    std::cout << buf << " [" << level << "]    " << msg << std::endl;
}

void signal_handler(int signum) {
    log("INFO", "Caught signal " + std::to_string(signum) + ". Shutting down manager...");
    running = false;

    std::lock_guard<std::mutex> lock(node_mutex);
    for (auto &[id, node] : nodes) {
        std::string shutdown_msg = "SHUTDOWN";
        sockaddr_in node_addr{};
        node_addr.sin_family = AF_INET;
        node_addr.sin_port = htons(node.port);
        inet_pton(AF_INET, node.ip.c_str(), &node_addr.sin_addr);

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd >= 0 && connect(sockfd, (sockaddr *)&node_addr, sizeof(node_addr)) == 0) {
            send(sockfd, shutdown_msg.c_str(), shutdown_msg.size(), 0);
            close(sockfd);
        }
    }

    log("INFO", "Manager: Shutdown complete.");
    exit(0);
}

namespace ManagerConfig {
    int MANAGER_LISTEN_PORT = 5000;
    int HEARTBEAT_PING_INTERVAL_SEC = 2;
    int NODE_UNRESPONSIVE_TIMEOUT_MS = 10000; // 10 seconds
    std::string LOG_FILE_NAME = "manager.log";
}

namespace ManagerHeartbeat {

    void send_heartbeat_ping(const std::string& ip, int port, const std::string& nodeId) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            log("ERROR", "Heartbeat: Failed to create socket for ping to " + nodeId);
            return;
        }

        sockaddr_in node_addr{};
        node_addr.sin_family = AF_INET;
        node_addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &node_addr.sin_addr);

        struct timeval tv;
        tv.tv_sec = ManagerConfig::NODE_UNRESPONSIVE_TIMEOUT_MS / 1000;
        tv.tv_usec = (ManagerConfig::NODE_UNRESPONSIVE_TIMEOUT_MS % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        if (connect(sock, (sockaddr *)&node_addr, sizeof(node_addr)) < 0) {
            log("WARN", "Heartbeat: Node " + nodeId + " at " + ip + ":" + std::to_string(port) + " unresponsive or disconnected (connect failed).");
            {
                std::lock_guard<std::mutex> lock(node_mutex);
                if (nodes.count(nodeId)) {
                    nodes[nodeId].available = false; 
                }
            }
            close(sock);
            return;
        }

        std::string ping_msg = "PING\n";
        send(sock, ping_msg.c_str(), ping_msg.size(), 0);

        char response_buf[16];
        ssize_t bytes_read = recv(sock, response_buf, sizeof(response_buf) - 1, 0);
        if (bytes_read > 0) {
            response_buf[bytes_read] = '\0';
            std::string response(response_buf);
            if (response.rfind("PONG", 0) == 0) {
                std::lock_guard<std::mutex> lock(node_mutex);
                if (nodes.count(nodeId)) {
                    nodes[nodeId].available = true;
                    nodes[nodeId].last_cpu_update = std::chrono::steady_clock::now();
                    nodes[nodeId].last_mem_update = std::chrono::steady_clock::now();
                    nodes[nodeId].last_heartbeat_pong = std::chrono::steady_clock::now();
                }
            } else {
                log("WARN", "Heartbeat: Node " + nodeId + " sent unexpected response: '" + response + "'");
                std::lock_guard<std::mutex> lock(node_mutex);
                if (nodes.count(nodeId)) {
                    nodes[nodeId].available = false;
                }
            }
        } else {
            log("WARN", "Heartbeat: Node " + nodeId + " failed to respond to ping (recv timeout/error).");
            {
                std::lock_guard<std::mutex> lock(node_mutex);
                if (nodes.count(nodeId)) {
                    nodes[nodeId].available = false;
                }
            }
        }
        close(sock);
    }

    void monitor_nodes_heartbeats() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(ManagerConfig::HEARTBEAT_PING_INTERVAL_SEC));

            std::map<std::string, NodeInfo> current_nodes_copy;
            {
                std::lock_guard<std::mutex> lock(node_mutex);
                current_nodes_copy = nodes;
            }

            for (const auto& pair : current_nodes_copy) {
                const auto& node_id = pair.first;
                const auto& node_info = pair.second;

                auto now = steady_clock::now();
                auto time_since_last_contact = duration_cast<milliseconds>(now - node_info.last_heartbeat_pong).count();

                if (time_since_last_contact > ManagerConfig::NODE_UNRESPONSIVE_TIMEOUT_MS) {
                    std::lock_guard<std::mutex> lock(node_mutex);
                    if (nodes.count(node_id) && nodes[node_id].available) { 
                        log("WARN", "Heartbeat: Node " + node_id + " unresponsive for " + std::to_string(time_since_last_contact) + "ms. Marking as unavailable.");
                        nodes[node_id].available = false;
                    }
                } else if (node_info.available) {
                    std::thread(send_heartbeat_ping, node_info.ip, node_info.port, node_id).detach();
                }
            }
        }
    }

} 

void assign_tasks() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::string task_to_assign;
        std::string best_node_id;
        double min_combined_load = 201.0;
        
        { // Lock task_mutex to safely access task_queue and tasks map
            std::lock_guard<std::mutex> lock(task_mutex);

            for (auto &[task_id, entry] : tasks) {
                if (entry.status == TaskStatus::ASSIGNED) {
                    std::lock_guard<std::mutex> nlock(node_mutex);
                  
                    if (nodes.find(entry.assigned_node) == nodes.end() || !nodes[entry.assigned_node].available) {
                        log("INFO", "Re-queuing task " + task_id + " from unavailable node " + entry.assigned_node);
                        entry.status = TaskStatus::QUEUED;
                        entry.assigned_node.clear();
                        entry.requeued_count++; 
                        task_queue.push(task_id);
                    }
                }
            }
            
            if (task_queue.empty()) {
                continue; 
            }
            
            task_to_assign = task_queue.front();

            if (tasks.count(task_to_assign) && tasks[task_to_assign].status == TaskStatus::COMPLETED) {
                task_queue.pop();
                continue;
            }
        }

        NodeInfo* best_node_ptr = nullptr;
        auto now = steady_clock::now();

        { 
            std::lock_guard<std::mutex> nlock(node_mutex);
            for (auto &[id, node] : nodes) {
                auto time_since_last_contact = duration_cast<milliseconds>(now - node.last_heartbeat_pong).count();

                if (node.available && time_since_last_contact < ManagerConfig::NODE_UNRESPONSIVE_TIMEOUT_MS) {
                    double current_combined_load = (node.cpu_usage + node.mem_usage) / 2.0;
                    if (current_combined_load < min_combined_load) {
                        min_combined_load = current_combined_load;
                        best_node_ptr = &node;
                        best_node_id = id;
                    }
                }
            }
        } 

        if (!best_node_ptr) {
            log("INFO", "No available node found to assign task " + task_to_assign + ". Will retry.");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        NodeInfo target_node_info_copy;
        {
            std::lock_guard<std::mutex> nlock(node_mutex);
            if (nodes.count(best_node_id)) {
                target_node_info_copy = nodes[best_node_id];
            } else {
                log("WARN", "Best node " + best_node_id + " disappeared before assignment. Re-evaluating.");
                continue; 
            }
        }

        sockaddr_in node_addr{};
        node_addr.sin_family = AF_INET;
        node_addr.sin_port = htons(target_node_info_copy.port);
        inet_pton(AF_INET, target_node_info_copy.ip.c_str(), &node_addr.sin_addr);

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        struct timeval tv_connect;
        tv_connect.tv_sec = 1; // 1 second connect timeout
        tv_connect.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv_connect, sizeof tv_connect);

        if (sockfd < 0 || connect(sockfd, (sockaddr *)&node_addr, sizeof(node_addr)) < 0) {
            log("ERROR", "Manager: Failed to connect to node " + target_node_info_copy.id + " for task assignment. Marking unavailable.");
            close(sockfd);
            {
                std::lock_guard<std::mutex> nlock(node_mutex);
                if (nodes.count(target_node_info_copy.id)) {
                    nodes[target_node_info_copy.id].available = false;
                }
            }
            // Re-queue the task if it was assigned to this node and not completed
            {
                std::lock_guard<std::mutex> tlock(task_mutex);
                auto& entry = tasks[task_to_assign];
                if (entry.status == TaskStatus::ASSIGNED && entry.assigned_node == target_node_info_copy.id) {
                    log("INFO", "Re-queuing task " + task_to_assign + " due to connection failure to " + target_node_info_copy.id);
                    entry.status = TaskStatus::QUEUED;
                    entry.assigned_node.clear();
                    entry.requeued_count++; // INCREMENTED HERE
                    task_queue.push(task_to_assign);
                }
            }
            continue;
        }

        { // Lock task_mutex for modification
            std::lock_guard<std::mutex> tlock(task_mutex);
            auto &entry = tasks[task_to_assign];
            
            // MODIFIED LOGGING LOGIC HERE
            std::string log_prefix = "Assigned ";
            if (entry.requeued_count > 0) {
                log_prefix = "Reassigned ";
            }
            log("INFO", log_prefix + task_to_assign + " to " + target_node_info_copy.id + " (Combined Load: " + std::to_string(min_combined_load) + "%).");
            // END MODIFIED LOGGING LOGIC

            entry.assigned_node = target_node_info_copy.id;
            entry.status = TaskStatus::ASSIGNED;

            task_queue.pop(); // Pop from queue after successful assignment attempt
        } // task_mutex released

        send(sockfd, task_to_assign.c_str(), task_to_assign.size(), 0);
        close(sockfd);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void handle_node(int client_sock) {
    char buffer[1024] = {0};
    ssize_t read_len = read(client_sock, buffer, sizeof(buffer)-1);
    buffer[read_len] = '\0';
    std::istringstream iss(buffer);
    std::string command, node_id;
    int port;
    iss >> command >> node_id >> port;

    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getpeername(client_sock, (sockaddr *)&addr, &len);
    std::string ip = inet_ntoa(addr.sin_addr);

    if (command == "REGISTER") {
        NodeInfo node{node_id, ip, port, client_sock, true, 100.0, 100.0, steady_clock::now(), steady_clock::now(), steady_clock::now()};
        {
            std::lock_guard<std::mutex> lock(node_mutex);
            nodes[node_id] = node;
        }
        log("INFO", "Node " + node_id + " connected from " + ip + ":" + std::to_string(port));
    } else {
        log("ERROR", "Unknown command from node: " + command);
        close(client_sock);
        return;
    }

    char recv_buf[1024];
    struct timeval tv;
    tv.tv_sec = ManagerConfig::NODE_UNRESPONSIVE_TIMEOUT_MS / 1000;
    tv.tv_usec = (ManagerConfig::NODE_UNRESPONSIVE_TIMEOUT_MS % 1000) * 1000;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    while (running) {
        ssize_t len = recv(client_sock, recv_buf, sizeof(recv_buf) - 1, 0);

        if (len > 0) {
            recv_buf[len] = '\0';
            std::string msg(recv_buf);
            std::istringstream stream(msg);
            std::string line;
            while (std::getline(stream, line, '\n')) {
                if (line.rfind("TASK_DONE ", 0) == 0) {
                    std::string task = line.substr(10);
                    std::lock_guard<std::mutex> lock(task_mutex);
                    auto it = tasks.find(task);
                    if (it != tasks.end()) {
                        it->second.status = TaskStatus::COMPLETED;
                        log("INFO", "Manager: Task " + task + " marked as completed by " + node_id);
                    } else {
                        log("WARN", "Manager: Received completion for unknown or already completed task: " + task);
                    }
                } else if (line.rfind("CPU_USAGE ", 0) == 0) {
                    double usage = std::stod(line.substr(10));
                    {
                        std::lock_guard<std::mutex> lock(node_mutex);
                        if (nodes.count(node_id)) {
                             nodes[node_id].cpu_usage = usage;
                             nodes[node_id].last_cpu_update = steady_clock::now();
                             nodes[node_id].available = true;
                             nodes[node_id].last_heartbeat_pong = steady_clock::now();
                        }
                    }
                    log("INFO", "Manager: Updated CPU usage for " + node_id + ": " + std::to_string(usage) + "%");
                } else if (line.rfind("MEM_USAGE ", 0) == 0) {
                    double usage = std::stod(line.substr(10));
                    {
                        std::lock_guard<std::mutex> lock(node_mutex);
                        if (nodes.count(node_id)) {
                             nodes[node_id].mem_usage = usage;
                             nodes[node_id].last_mem_update = steady_clock::now();
                             nodes[node_id].available = true;
                             nodes[node_id].last_heartbeat_pong = steady_clock::now();
                        }
                    }
                    log("INFO", "Manager: Updated Memory usage for " + node_id + ": " + std::to_string(usage) + "%");
                } else {
                    log("WARN", "Manager: Unrecognized message from " + node_id + ": '" + line + "'");
                }
            }
        } else if (len == 0) {
            log("WARN", "Node " + node_id + " disconnected unexpectedly (recv returned 0).");
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              
            } else {
                log("ERROR", "Node " + node_id + " socket error: " + std::strerror(errno));
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }


    {
        std::lock_guard<std::mutex> nlock(node_mutex);
        if (nodes.count(node_id)) {
            nodes.erase(node_id);
        }
    }

    {
        std::lock_guard<std::mutex> tlock(task_mutex);
        for (auto &[task_id, entry] : tasks) {
            if (entry.assigned_node == node_id && entry.status == TaskStatus::ASSIGNED) {
                log("INFO", "Re-queuing task " + task_id + " from disconnected node " + node_id);
                entry.status = TaskStatus::QUEUED;
                entry.assigned_node.clear();
                entry.requeued_count++; 
                task_queue.push(task_id);
            }
        }
    }

    close(client_sock);
    log("INFO", "Cleaned up resources for disconnected node " + node_id);
}

void handle_client(int client_sock) {
    char buffer[1024] = {0};
    ssize_t valread = read(client_sock, buffer, sizeof(buffer));
    std::string task_input(buffer, valread);
    std::istringstream iss(task_input);
    std::string line;
    {
        std::lock_guard<std::mutex> lock(task_mutex);
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            size_t end = line.find_last_not_of(" \t\n\r\f\v");
            if (std::string::npos != end) {
                line = line.substr(0, end + 1);
            } else {
                line.clear();
                continue;
            }

            if (tasks.count(line) && tasks[line].status == TaskStatus::COMPLETED) {
                log("INFO", "Ignoring already completed task: " + line);
                continue;
            }
         
            if (!tasks.count(line) || (tasks[line].status != TaskStatus::QUEUED && tasks[line].status != TaskStatus::ASSIGNED)) {
                tasks[line] = TaskEntry{line, TaskStatus::QUEUED, "", 0}; 
                task_queue.push(line);
                log("INFO", "Received task: " + line);
            } else {
                log("INFO", "Task " + line + " is already in queue or assigned.");
            }
        }
    }
    close(client_sock);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::ofstream log_file(ManagerConfig::LOG_FILE_NAME, std::ios::out | std::ios::app);
    TeeBuf tee_buf(std::cout.rdbuf(), log_file.rdbuf());
    std::ostream dual_out(&tee_buf);
    std::cout.rdbuf(dual_out.rdbuf());

    int port = ManagerConfig::MANAGER_LISTEN_PORT;
    if (argc == 2) {
        try {
            port = std::stoi(argv[1]);
        } catch (const std::exception& e) {
            log("ERROR", "Invalid port number provided: " + std::string(argv[1]) + ". Using default " + std::to_string(ManagerConfig::MANAGER_LISTEN_PORT));
            port = ManagerConfig::MANAGER_LISTEN_PORT;
        }
    }

    log("INFO", "Manager starting...");
    log("INFO", "Heartbeat Ping Interval: " + std::to_string(ManagerConfig::HEARTBEAT_PING_INTERVAL_SEC) + " seconds");
    log("INFO", "Node Unresponsive Timeout: " + std::to_string(ManagerConfig::NODE_UNRESPONSIVE_TIMEOUT_MS) + " ms");


    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    log("INFO", "Manager listening on 0.0.0.0:" + std::to_string(port));

    std::thread assign_thread(assign_tasks);
    std::thread heartbeat_monitor_thread(ManagerHeartbeat::monitor_nodes_heartbeats);

    while (running) {
        sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);
        int new_socket = accept(server_fd, (sockaddr *)&client_addr, &addrlen);
        if (new_socket < 0) {
            if (errno == EINTR) {
                continue;
            }
            log("ERROR", "Accept failed: " + std::string(strerror(errno)));
            if (!running) break;
            continue;
        }

        char buffer[1024] = {0};
        ssize_t peek_len = recv(new_socket, buffer, sizeof(buffer), MSG_PEEK);
        if (peek_len <= 0) {
            close(new_socket);
            continue;
        }
        std::string peek(buffer, peek_len);

        if (peek.rfind("REGISTER", 0) == 0) {
            std::thread(handle_node, new_socket).detach();
        } else {
            std::thread(handle_client, new_socket).detach();
        }
    }

    if (assign_thread.joinable()) {
        assign_thread.join();
    }
    if (heartbeat_monitor_thread.joinable()) {
        heartbeat_monitor_thread.join();
    }
    
    close(server_fd);
    log("INFO", "Manager gracefully shut down.");
    return 0;
}