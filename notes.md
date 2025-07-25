**Steps to Use:**

1. Open Ubuntu (WSL Terminal)

*Terminal 1:*

2. cd ClusterResourceManager
3. make clean
   
   Cleans existing build and log directories.
5. make
   
   This will create the build directory and the executables (cluster\_manager, node\_agent, task\_client) inside it. It will also create the logs directory.
7. ./build/manager
8. Open multiple new terminal windows for each node agent (Ex:3 nodes)


*Terminals 2,3,4 respectively:*

7. cd ClusterResourceManager
8. ./build/node\_agent node1 127.0.0.1 5000 9001
   
   ./build/node\_agent node2 127.0.0.1 5000 9002
   
   ./build/node\_agent node3 127.0.0.1 5000 9003
   
   (Each node will also log to its own file)


*Terminal 5:*

9. ./build/client 127.0.0.1 5000 10
    
   This will submit 10 tasks. It will also schedule, and assign it to nodes.


11. To Demonstrate Failover:
    - Go to one of the Node Agent terminals (e.g., node2) and press Ctrl+C to terminate the process.

    - In the Manager Terminal, you will see a log message indicating that the connection to node2 was lost (e.g., ERROR: Lost connection to Node node2).
      It will then automatically detect the failure, mark node2 as inactive, and reassign any incomplete tasks to the next available healthy node (e.g., node1 or node3).
      A message like Reassigning task TASK_X from failed Node node2 to Node node3 confirms the failover and recovery mechanism is working.

12. Cleanup:
    - Press Ctrl+C in all terminals.

    - Run make clean to remove builds and logs
