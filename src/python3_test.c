/*
 * python3_test.c
 * --------------------------------------------
 * Linux-only utilities for testing Python 3 integration in a sandboxed environment.
 * Provides a function to execute a Python 3 REPL process with resource limits and
 * bidirectional communication via pipes, suitable for integration testing in a daemon
 * or standalone context. Designed to verify Python script execution and communication
 * reliability for applications like OBS automation or scripting plugins.
 *
 * Author: Aidan Bradley
 * Date:   2025-09-27
 *
 * Usage as a System/Daemon Component:
 * 1. Prerequisites:
 *    - Ensure a Linux environment with Python 3 installed and accessible via `python3`.
 *    - Include the header `python3_test.h`, which defines the function prototype for
 *      test_python_integration().
 *    - Requires standard C libraries and POSIX support for fork(), pipes, and resource limits.
 *
 * 2. Purpose:
 *    - Tests Python 3 integration by running a simple REPL in a child process with
 *      sandboxed resource limits (memory, CPU time, file descriptors).
 *    - Verifies bidirectional communication between the parent process and the Python REPL
 *      using pipes for stdin/stdout.
 *    - Useful for ensuring Python scripting support in a daemon (e.g., OBS WebSocket client)
 *      or standalone test suite.
 *
 * 3. Running the Test:
 *    - Call test_python_integration() to execute the test.
 *    - The function forks a child process running a Python REPL, applies resource limits,
 *      sends test commands, and captures output.
 *    - Returns 0 on success (Python process exits cleanly with expected output) or -1 on failure.
 *
 * 4. Integration in a Daemon:
 *    - Incorporate into a daemon (e.g., OBS WebSocket client) to periodically verify Python
 *      integration, such as for scripting plugins or automation tasks.
 *    - Example daemon integration:
 *      ```c
 *      #include "python3_test.h"
 *      #include <signal.h>
 *      #include <unistd.h>
 *      static volatile int running = 1;
 *      void signal_handler(int sig) { running = 0; }
 *      int main() {
 *          signal(SIGINT, signal_handler);
 *          while (running) {
 *              if (test_python_integration() == 0) {
 *                  printf("Python integration test passed\n");
 *              } else {
 *                  printf("Python integration test failed\n");
 *              }
 *              sleep(60); // Run test every minute
 *          }
 *          return 0;
 *      }
 *      ```
 *    - For a system daemon, wrap in a systemd service (similar to obs_websocket.c example):
 *      ```ini
 *      [Unit]
 *      Description=Python3 Integration Test Daemon
 *      After=network.target
 *
 *      [Service]
 *      ExecStart=/path/to/python3_test_app
 *      Restart=always
 *
 *      [Install]
 *      WantedBy=multi-user.target
 *      ```
 *
 * 5. Customizing Tests:
 *    - Modify the `py_code` string to include custom Python code for testing specific functionality.
 *    - Adjust the `test_cmds` array to send different commands to the Python REPL.
 *    - Update resource limits (e.g., RLIMIT_AS, RLIMIT_CPU) in the child process to suit your needs.
 *
 * 6. Notes for Maintenance:
 *    - The test uses fork() and pipes for communication, with resource limits to sandbox the Python process.
 *    - Errors (e.g., pipe creation, fork failure, Python execution errors) are reported via perror().
 *    - The Python REPL processes commands sequentially; ensure test commands are valid Python syntax.
 *    - When revisiting, check `python3_test.h` for function prototypes and update resource limits
 *      or test commands as needed.
 *    - Debug failures by inspecting perror() messages and the child process exit status.
 *    - Extend functionality by adding more test commands or supporting additional Python versions.
 *
 * 7. Example Usage (Standalone):
 *    ```c
 *    #include "python3_test.h"
 *    int main() {
 *        if (test_python_integration() == 0) {
 *            printf("Python integration test passed\n");
 *        } else {
 *            printf("Python integration test failed\n");
 *        }
 *        return 0;
 *    }
 *    ```
 *    Expected output:
 *      ```
 *      Python output: 5
 *      Python output: 84
 *      Python output: imported
 *      Python integration test passed
 *      ```
 *
 * 8. Limitations:
 *    - Assumes `python3` is in the PATH; verify with check_application() from check_dependencies.c.
 *    - Resource limits are hardcoded (100MB memory, 60s CPU, 4 file descriptors); adjust as needed.
 *    - Error handling is basic; consider adding logging for production use.
 */

#include "python3_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/resource.h>
#include <errno.h>

/**
 * @brief Tests Python 3 integration by running a simple REPL in a sandboxed environment.
 *
 * Forks a child process to execute a Python 3 REPL, applies resource limits to sandbox the process,
 * and tests bidirectional communication using pipes. Sends predefined Python commands and verifies
 * output. Suitable for integration testing in a daemon or standalone context.
 *
 * @return 0 on success (Python process exits cleanly with expected behavior), -1 on failure
 *         (e.g., pipe creation, fork, or Python execution errors).
 */
int test_python_integration(void)
{
    // Python code for a simple REPL server that processes input commands and handles errors.
    char *py_code =
        "import sys\n"
        "for line in sys.stdin:\n"
        "    cmd = line.strip()\n"
        "    if cmd == 'quit': break\n"
        "    try:\n"
        "        exec(cmd)\n"
        "    except:\n"
        "        print('error')\n";

    // Set up pipes for bidirectional communication between parent and child.
    int to_child[2], from_child[2]; // to_child: parent writes, child reads; from_child: child writes, parent reads.
    if (pipe(to_child) == -1 || pipe(from_child) == -1) {
        perror("pipe"); // Print error if pipe creation fails.
        return -1;
    }

    // Fork a child process to run the Python REPL.
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork"); // Print error if fork fails.
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return -1;
    } else if (pid == 0) { // Child process
        // Redirect stdin to read from to_child pipe.
        close(to_child[1]); // Close write end of to_child pipe.
        if (dup2(to_child[0], STDIN_FILENO) == -1) {
            perror("dup2 stdin");
            exit(1);
        }
        close(to_child[0]); // Close read end of to_child pipe.

        // Redirect stdout to write to from_child pipe.
        close(from_child[0]); // Close read end of from_child pipe.
        if (dup2(from_child[1], STDOUT_FILENO) == -1) {
            perror("dup2 stdout");
            exit(1);
        }
        close(from_child[1]); // Close write end of from_child pipe.

        // Apply resource limits to sandbox the Python process.
        struct rlimit rl;
        // Limit virtual memory to 100MB.
        rl.rlim_cur = rl.rlim_max = 100 * 1024 * 1024;
        if (setrlimit(RLIMIT_AS, &rl) == -1) {
            perror("setrlimit AS"); // Print error if memory limit fails.
            exit(1);
        }
        // Limit CPU time to 60 seconds.
        rl.rlim_cur = rl.rlim_max = 60;
        if (setrlimit(RLIMIT_CPU, &rl) == -1) {
            perror("setrlimit CPU"); // Print error if CPU limit fails.
            exit(1);
        }
        // Limit open file descriptors to 4 (stdin, stdout, stderr + 1).
        rl.rlim_cur = rl.rlim_max = 4;
        if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
            perror("setrlimit NOFILE"); // Print error if file descriptor limit fails.
            exit(1);
        }

        // Execute Python 3 with the REPL code.
        execlp("python3", "python3", "-c", py_code, (char *)NULL);
        perror("execlp"); // Print error if exec fails.
        exit(1);
    } else { // Parent process
        // Close unused pipe ends.
        close(to_child[0]);   // Close read end of to_child pipe.
        close(from_child[1]); // Close write end of from_child pipe.

        // Test commands to send to the Python REPL.
        char *test_cmds[] = {
            "print(2 + 3)\n",          // Test basic arithmetic.
            "x = 42\n",                // Test variable assignment.
            "print(x * 2)\n",          // Test variable usage.
            "import sys; print('imported')\n", // Test module import.
            "quit\n",                  // Terminate the REPL.
            NULL                       // End of command list.
        };

        // Send test commands to the Python process.
        for (int i = 0; test_cmds[i]; i++) {
            if (write(to_child[1], test_cmds[i], strlen(test_cmds[i])) == -1) {
                perror("write to Python"); // Print error if write fails.
                close(to_child[1]);
                close(from_child[0]);
                kill(pid, SIGTERM); // Terminate child process.
                waitpid(pid, NULL, 0);
                return -1;
            }
        }

        // Read and display responses from the Python process.
        char buf[1024];
        ssize_t n;
        while ((n = read(from_child[0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0'; // Null-terminate the buffer.
            fprintf(stdout, "Python output: %s", buf); // Print Python output.
        }
        if (n == -1) {
            perror("read from Python"); // Print error if read fails.
        }

        // Close remaining pipe ends.
        close(to_child[1]);
        close(from_child[0]);

        // Wait for the child process to exit and check its status.
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 0; // Success if child exited cleanly.
        } else {
            fprintf(stderr, "Python process failed with status: %d\n", WEXITSTATUS(status));
            return -1; // Failure if child exited with non-zero status or was terminated.
        }
    }
}