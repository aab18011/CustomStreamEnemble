/*
 * python3_test.h
 * --------------------------------------------
 * Header for Python3 integration testing utilities.
 * Provides functionality to test Python execution and communication.
 *
 * Author: Aidan Bradley
 * Date:   9/27/25
 */

#ifndef PYTHON3_TEST_H
#define PYTHON3_TEST_H

/**
 * @brief Test Python3 integration by running a simple REPL and communicating with it.
 *
 * Executes a Python process in a sandboxed environment with resource limits
 * and tests bidirectional communication using a pipe-based REPL.
 *
 * @return 0 on success, -1 on failure.
 */
int test_python_integration(void);

#endif /* PYTHON3_TEST_H */