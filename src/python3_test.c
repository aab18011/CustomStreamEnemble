/*
 * python3_test.c
 * --------------------------------------------
 * Implementation of Python3 integration testing utilities.
 * Tests Python execution in a sandboxed environment with communication.
 *
 * Author: Aidan Bradley
 * Date:   9/27/25
 */

#include "python3_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <errno.h>

/**
 * @brief Test Python3 integration by running a simple REPL and communicating with it.
 *
 * Executes a Python process in a sandboxed environment with resource limits
 * and tests bidirectional communication using a pipe-based REPL.
 *
 * @return 0 on success, -1 on failure.
 */
int test_python_integration(void)
{
	// Python code for a simple REPL server
	char *py_code =
		"import sys\n"
		"for line in sys.stdin:\n"
		"    cmd = line.strip()\n"
		"    if cmd == 'quit': break\n"
		"    try:\n"
		"        exec(cmd)\n"
		"    except:\n"
		"        print('error')\n";

	// Set up pipes for bidirectional communication
	int to_child[2], from_child[2];
	if (pipe(to_child) == -1 || pipe(from_child) == -1) {
		perror("pipe");
		return -1;
	}

	pid_t pid = fork();
	if (pid == -1) {
		perror("fork");
		return -1;
	} else if (pid == 0) { // Child process
		// Set up stdin/stdout
		close(to_child[1]);
		dup2(to_child[0], STDIN_FILENO);
		close(to_child[0]);
		close(from_child[0]);
		dup2(from_child[1], STDOUT_FILENO);
		close(from_child[1]);

		// Sandbox: limit resources
		struct rlimit rl;
		// Limit virtual memory to 100MB
		rl.rlim_cur = rl.rlim_max = 100 * 1024 * 1024;
		if (setrlimit(RLIMIT_AS, &rl) == -1) {
			perror("setrlimit AS");
		}
		// Limit CPU time to 60 seconds
		rl.rlim_cur = rl.rlim_max = 60;
		if (setrlimit(RLIMIT_CPU, &rl) == -1) {
			perror("setrlimit CPU");
		}
		// Limit open files to 4 (stdin, stdout, stderr +1)
		rl.rlim_cur = rl.rlim_max = 4;
		if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
			perror("setrlimit NOFILE");
		}

		// Execute Python
		execlp("python3", "python3", "-c", py_code, (char *)NULL);
		perror("execlp");
		exit(1);
	} else { // Parent process
		close(to_child[0]);
		close(from_child[1]);

		// Test communication
		char *test_cmds[] = {
			"print(2 + 3)\n",
			"x = 42\n",
			"print(x * 2)\n",
			"import sys; print('imported')\n",
			"quit\n",
			NULL
		};

		for (int i = 0; test_cmds[i]; i++) {
			if (write(to_child[1], test_cmds[i], strlen(test_cmds[i])) == -1) {
				perror("write to Python");
				close(to_child[1]);
				close(from_child[0]);
				return -1;
			}
		}

		// Read responses
		char buf[1024];
		ssize_t n;
		while ((n = read(from_child[0], buf, sizeof(buf) - 1)) > 0) {
			buf[n] = '\0';
			fprintf(stdout, "Python output: %s", buf);
		}
		if (n == -1) {
			perror("read from Python");
		}

		close(to_child[1]);
		close(from_child[0]);

		// Wait for child
		int status;
		waitpid(pid, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			return 0;
		} else {
			return -1;
		}
	}
}