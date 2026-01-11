#!/bin/bash
# t_compile.sh - Compile-only tests for all example files
#
# These tests verify that all examples compile successfully
# without running them (useful for testing the compiler itself)

# Server programs that compile but run indefinitely
test_compile "$EXAMPLES_DIR/web_server.strada" "web_server" "Web server"
test_compile "$EXAMPLES_DIR/simple_select_server.strada" "simple_select_server" "Select server"
test_compile "$EXAMPLES_DIR/prefork_server.strada" "prefork_server" "Prefork server"
test_compile "$EXAMPLES_DIR/test_socket_server.strada" "test_socket_server" "Socket server"
