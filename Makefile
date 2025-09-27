# Makefile for Unified Streaming System
CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -g
LDFLAGS = 
SRC_DIR = src
INCLUDE_DIR = include
OBJ_DIR = obj
BIN_DIR = bin

# List of source files
SOURCES = $(wildcard $(SRC_DIR)/*.c)
# Generate object file names
OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
# Executable name
EXEC = $(BIN_DIR)/streaming_system

# Default target
all: $(EXEC)

# Link object files to create executable
$(EXEC): $(OBJECTS) | $(BIN_DIR)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

# Compile source files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Create directories if they don't exist
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Clean up
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

# Phony targets
.PHONY: all clean
