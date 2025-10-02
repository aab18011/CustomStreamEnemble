# Makefile for ROC System Project
# Targets:
#   all: Build all executables
#   clean: Remove build artifacts
#   install: Install executables to /usr/local/bin (requires root)
#   check-prereqs: Check for required libraries and tools

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lpthread -lcjson

SRCDIR = src
INCDIR = include
BINDIR = bin
OBJDIR = obj

# Create directories if they don't exist
$(shell mkdir -p $(BINDIR) $(OBJDIR))

# Common objects (shared libraries compiled from source)
COMMON_OBJS = \
	$(OBJDIR)/cJSON.o

# Main controller sources and objects
MAIN_SRCS = \
	$(SRCDIR)/main.c \
	$(SRCDIR)/depcheck.c \
	$(SRCDIR)/modulecheck.c \
	$(SRCDIR)/lan_check.c \
	$(SRCDIR)/wlan_check.c \
	$(SRCDIR)/python3_test.c

MAIN_OBJS = $(MAIN_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o) $(COMMON_OBJS)

# Videopipe sources and objects
VIDEOPIPE_SRCS = \
	$(SRCDIR)/videopipe.c

VIDEOPIPE_OBJS = $(VIDEOPIPE_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o) $(COMMON_OBJS)

# V4L2 loopback installer sources and objects (no cJSON dependency)
V4L2_SRCS = \
	$(SRCDIR)/v4l2loopback_mod_install.c

V4L2_OBJS = $(V4L2_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# Executables
MAIN_EXEC = $(BINDIR)/main_controller
VIDEOPIPE_EXEC = $(BINDIR)/videopipe
V4L2_EXEC = $(BINDIR)/v4l2loopback_mod_install

# Header dependencies
HEADERS = $(wildcard $(INCDIR)/*.h)

all: check-prereqs $(MAIN_EXEC) $(VIDEOPIPE_EXEC) $(V4L2_EXEC)

# Prerequisite checking
REQUIRED_TOOLS = gcc make
REQUIRED_LIBS = libcjson-dev

DISTRO = $(shell if [ -f /etc/debian_version ]; then echo "debian"; elif [ -f /etc/redhat-release ]; then echo "redhat"; elif [ -f /etc/arch-release ]; then echo "arch"; else echo "unknown"; fi)

.PHONY: check-prereqs all clean install

check-prereqs:
	@echo "Checking prerequisites for compilation..."
	@echo "Distribution: $(DISTRO)"
	@echo "\nChecking required tools: $(REQUIRED_TOOLS)"
	@for tool in $(REQUIRED_TOOLS); do \
		if command -v $$tool >/dev/null 2>&1; then \
			echo "$$tool: OK"; \
		else \
			echo "ERROR: $$tool not found. Install it using:"; \
			if [ "$(DISTRO)" = "debian" ]; then \
				echo "  sudo apt-get install $$tool"; \
			elif [ "$(DISTRO)" = "redhat" ]; then \
				echo "  sudo dnf install $$tool"; \
			elif [ "$(DISTRO)" = "arch" ]; then \
				echo "  sudo pacman -S $$tool"; \
			else \
				echo "  Install $$tool manually for your distribution"; \
			fi; \
			exit 1; \
		fi; \
	done
	@echo "\nChecking required libraries: $(REQUIRED_LIBS)"
	@if [ "$(DISTRO)" = "debian" ]; then \
		for lib in $(REQUIRED_LIBS); do \
			if dpkg -l | grep -q $$lib 2>/dev/null; then \
				echo "$$lib: OK"; \
			else \
				echo "ERROR: $$lib not found. Install it using:"; \
				echo "  sudo apt-get install $$lib"; \
				exit 1; \
			fi; \
		done; \
	elif [ "$(DISTRO)" = "redhat" ]; then \
		for lib in $(REQUIRED_LIBS); do \
			REDHAT_LIB=$$(echo $$lib | sed 's/libcjson-dev/libcjson/'); \
			if rpm -q $$REDHAT_LIB >/dev/null 2>&1; then \
				echo "$$lib: OK"; \
			else \
				echo "ERROR: $$lib not found. Install it using:"; \
				echo "  sudo dnf install $$REDHAT_LIB"; \
				exit 1; \
			fi; \
		done; \
	elif [ "$(DISTRO)" = "arch" ]; then \
		for lib in $(REQUIRED_LIBS); do \
			ARCH_LIB=$$(echo $$lib | sed 's/libcjson-dev/cjson/'); \
			if pacman -Q $$ARCH_LIB >/dev/null 2>&1; then \
				echo "$$lib: OK"; \
			else \
				echo "ERROR: $$lib not found. Install it using:"; \
				echo "  sudo pacman -S $$ARCH_LIB"; \
				exit 1; \
			fi; \
		done; \
	else \
		echo "WARNING: Unknown distribution, cannot check libraries automatically."; \
		echo "Please ensure libcjson (or equivalent) is installed."; \
		exit 1; \
	fi
	@echo "\nAll prerequisites satisfied!"

# Rule to compile .c to .o with header dependencies
$(OBJDIR)/%.o: $(SRCDIR)/%.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@ || { echo "Compilation failed for $<"; exit 1; }

# Main controller executable
$(MAIN_EXEC): $(MAIN_OBJS)
	@echo "Linking $@..."
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) || { echo "Linking failed for $@"; exit 1; }

# Videopipe executable
$(VIDEOPIPE_EXEC): $(VIDEOPIPE_OBJS)
	@echo "Linking $@..."
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) || { echo "Linking failed for $@"; exit 1; }

# V4L2 loopback installer executable (no cJSON dependency)
$(V4L2_EXEC): $(V4L2_OBJS)
	@echo "Linking $@..."
	$(CC) $(CFLAGS) $^ -o $@ -lpthread || { echo "Linking failed for $@"; exit 1; }

clean:
	rm -rf $(OBJDIR)/*.o $(BINDIR)/*

install:
	@echo "Installing executables to /usr/local/bin..."
	cp $(MAIN_EXEC) /usr/local/bin/ || { echo "Failed to install $(MAIN_EXEC)"; exit 1; }
	cp $(VIDEOPIPE_EXEC) /usr/local/bin/ || { echo "Failed to install $(VIDEOPIPE_EXEC)"; exit 1; }
	cp $(V4L2_EXEC) /usr/local/bin/ || { echo "Failed to install $(V4L2_EXEC)"; exit 1; }