
# 🚀 Cluster Resource Manager

A **distributed cluster resource management system** implemented in **C++**, designed to efficiently schedule and assign tasks across multiple computing nodes. It emulates key aspects of real-world distributed systems — including **load-aware scheduling**, **fault-tolerant execution**, and **OS-level resource tracking** — making it an ideal project to demonstrate applied systems programming concepts.

---

## 🌟 Core Highlights

✅ **Dynamic Load Balancing**
Tasks are intelligently assigned to nodes based on real-time CPU and memory usage, ensuring optimal resource utilization and preventing bottlenecks.

✅ **Fault Tolerance & Recovery**
Robust failover mechanism detects node crashes using heartbeat signals and reassigns pending/incomplete tasks to healthy nodes, ensuring high availability.

✅ **Health Monitoring**
Node Agents periodically report their system stats (CPU, memory) via `/proc` interfaces, enabling the Manager to maintain an accurate global view of the cluster state.

✅ **Scalable Multi-threaded Design**
All components use **`std::thread`** for concurrent execution—handling simultaneous node connections, client requests, and internal scheduling tasks.

✅ **Detailed Logging**
Timestamps and structured logs for all major operations (task assignments, completions, node joins/leaves) aid in debugging and performance analysis.

---

## 🧠 Linux OS Concepts Demonstrated

This project is a practical showcase of foundational Linux systems programming:

| Concept                | Demonstrated By                                                                                   |
| ---------------------- | ------------------------------------------------------------------------------------------------- |
| **Memory Management**  | Node Agents read from `/proc/meminfo` to report available memory.                                 |
| **Virtual Memory**     | Observed through each task's execution in its own address space.                                  |
| **Processes**          | Each Manager and Node Agent runs as an independent process.                                       |
| **Scheduling**         | Manager uses a custom resource-aware scheduler; nodes reflect kernel scheduling via `/proc/stat`. |
| **Multithreading**     | All major actions (task execution, resource reporting) are handled via threads.                   |
| **IPC**                | Implemented using TCP sockets for communication between components.                               |
| **Socket Programming** | Comprehensive use of TCP for Manager↔Node, Client↔Manager interactions.                           |
| **File System Access** | Interactions with `/proc` for system metrics and log file generation.                             |

---

## ⚙️ System Architecture

The architecture consists of **three primary components**:

### 🧠 Manager (`manager.cpp`)

Acts as the **central coordinator** of the system.

* Accepts Node and Client connections.
* Maintains registry of active nodes with real-time resource stats.
* Manages a task queue and performs task scheduling.
* Detects node failures using heartbeats.
* Reassigns lost tasks upon node crashes.

> **Runs as a server**, listening for registration, heartbeats, and task submissions.

---

### 🧩 Node Agent (`node_agent.cpp`)

Simulates a **worker node**.

* Registers with the Manager on startup.
* Periodically sends CPU and memory usage.
* Executes tasks and confirms completion.
* Responds to heartbeats to indicate health.

> **Runs both as a client (to Manager)** and **as a task listener (mini-server)**.

---

### 📦 Client (`client.cpp`)

Represents an **external task submitter**.

* Connects to the Manager.
* Sends a batch of tasks for execution.

> **Acts as a client** in a client-server model.

---

## 🧮 Algorithms & Strategies

### ⚖️ Load-Aware Greedy + FCFS Scheduling

* Tasks are picked in **First-Come, First-Served** order.
* Dispatched to the node with **lowest CPU and memory usage**.
* Ensures fairness while optimizing resource efficiency.

### 🛡️ Reactive Fault Recovery

* Manager continuously monitors heartbeat timestamps.
* On node failure, **reassigns in-progress tasks** to healthy nodes.
* Guarantees no tasks are dropped even on crash.

### 🔄 Concurrent Execution

* Manager spawns new threads for:

  * Each client and node connection
  * Heartbeat monitoring
  * Task reassignment
* Node Agents spawn:

  * Resource usage updater
  * Task listener and executor

---

