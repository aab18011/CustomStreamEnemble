# Makefile for Unified Streaming System
CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99 -D_GNU_SOURCE -Iinclude
LDFLAGS = -pthread -lssl -lcrypto -lcjson -luuid

# Directory structure (current flat layout)
SRC_DIR = src
INCLUDE_DIR = include
OBJ_DIR = obj
BIN_DIR = bin

# Source files (all in src/)
SOURCES = $(wildcard $(SRC_DIR)/*.c)

# Object files 
OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Executable name
EXEC = $(BIN_DIR)/streaming_system

# Default target
all: check_deps $(EXEC)

# Check for required dependencies
check_deps:
	@echo "Checking dependencies..."
	@command -v pkg-config >/dev/null 2>&1 || (echo "Error: pkg-config not found" && exit 1)
	@pkg-config --exists openssl || (echo "Error: OpenSSL development libraries not found. Install libssl-dev" && exit 1)
	@pkg-config --exists libcjson || (echo "Warning: libcjson not found via pkg-config, trying manual check...")
	@test -f /usr/include/cjson/cJSON.h || test -f /usr/local/include/cjson/cJSON.h || (echo "Error: cJSON headers not found. Install libcjson-dev" && exit 1)
	@pkg-config --exists uuid || test -f /usr/include/uuid/uuid.h || (echo "Error: UUID library not found. Install uuid-dev" && exit 1)
	@echo "All dependencies found."

# Link object files to create executable
$(EXEC): $(OBJECTS) | $(BIN_DIR)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	@echo "Build complete: $@"

# Compile source files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Create directories if they don't exist
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Install system dependencies (Ubuntu/Debian)
install_deps_ubuntu:
	sudo apt-get update
	sudo apt-get install -y libssl-dev libcjson-dev uuid-dev pkg-config

# Install system dependencies (CentOS/RHEL/Fedora)
install_deps_redhat:
	sudo yum install -y openssl-devel cjson-devel libuuid-devel pkgconfig
	# Or for newer versions: sudo dnf install -y openssl-devel cjson-devel libuuid-devel pkgconfig

# Development build with debug symbols and sanitizers
debug: CFLAGS += -DDEBUG -fsanitize=address -fno-omit-frame-pointer
debug: LDFLAGS += -fsanitize=address
debug: $(EXEC)

# Release build with optimizations
release: CFLAGS += -O3 -DNDEBUG -flto
release: LDFLAGS += -flto -s
release: $(EXEC)

# Test build (compiles but doesn't link - useful for syntax checking)
test_compile: $(OBJECTS)
	@echo "Compilation test passed"

# Show what files will be compiled
show_files:
	@echo "=== Source Files ==="
	@echo "$(SOURCES)"
	@echo ""
	@echo "=== Object Files ==="
	@echo "$(OBJECTS)"
	@echo ""
	@echo "=== Executable ==="
	@echo "$(EXEC)"

# Clean up
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "Clean complete"

# Clean and rebuild
rebuild: clean all

# Show current configuration
show_config:
	@echo "=== Build Configuration ==="
	@echo "CC: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "Source directory: $(SRC_DIR)"
	@echo "Include directory: $(INCLUDE_DIR)"
	@echo "Object directory: $(OBJ_DIR)"
	@echo "Binary directory: $(BIN_DIR)"
	@echo "Target: $(EXEC)"

# Help target
help:
	@echo "Available targets:"
	@echo "  all              - Build the complete system (default)"
	@echo "  debug            - Build with debug flags and sanitizers"
	@echo "  release          - Build optimized release version"
	@echo "  test_compile     - Test compilation without linking"
	@echo "  check_deps       - Check for required system dependencies"
	@echo "  install_deps_ubuntu - Install dependencies on Ubuntu/Debian"
	@echo "  install_deps_redhat - Install dependencies on CentOS/RHEL/Fedora"
	@echo "  show_files       - Show source and object files"
	@echo "  clean            - Remove built files"
	@echo "  rebuild          - Clean and rebuild"
	@echo "  show_config      - Display current build configuration"
	@echo "  help             - Show this help message"

# Phony targets
.PHONY: all debug release test_compile check_deps install_deps_ubuntu install_deps_redhat show_files clean rebuild show_config help