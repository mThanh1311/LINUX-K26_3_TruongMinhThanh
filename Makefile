CC := gcc

# Compiler / Linker flags
CPPFLAGS := -Iinclude

CFLAGS := \
	-Wall \
	-Wextra \
	-Wpedantic \
	-std=gnu11 \
	-pthread

DEBUG_FLAGS := \
	-g \
	-O0 \
	-DDEBUG

# SQLite
LDLIBS := \
	-lsqlite3


# Directories
SRC_DIR := src
TOOLS_DIR := tools
TEST_DIR := test

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin

# Main gateway
TARGET := $(BIN_DIR)/server


# Sensor simulator
SENSOR_NODE_SRC := $(TOOLS_DIR)/sensor_node.c
SENSOR_NODE_TARGET := $(BIN_DIR)/sensor_node


# Protocol unit tests
TEST_PROTOCOLS_SRC := $(TEST_DIR)/test_protocols.c
TEST_PROTOCOLS_TARGET := $(BIN_DIR)/test_protocols

# Source files
SRC := \
	$(wildcard $(SRC_DIR)/*.c) \
	$(wildcard $(SRC_DIR)/protocols/*.c)


# Convert:
#
#   src/main.c
#   src/storage_manager.c
#   src/protocols/protocol_adapters.c
#
# into:
#
#   build/obj/main.o
#   build/obj/storage_manager.o
#   build/obj/protocols/protocol_adapters.o
#
OBJ := \
	$(patsubst \
		$(SRC_DIR)/%.c, \
		$(OBJ_DIR)/%.o, \
		$(SRC))


# Phony targets
.PHONY: \
	all \
	debug \
	run \
	clean \
	test \
	test-protocols \
	stress

# Default build
all: \
	$(TARGET) \
	$(SENSOR_NODE_TARGET)

# Gateway server
$(TARGET): $(OBJ)
	mkdir -p $(BIN_DIR)
	$(CC) \
		$(CFLAGS) \
		$(OBJ) \
		-o $@ \
		$(LDLIBS)

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) \
		$(CPPFLAGS) \
		$(CFLAGS) \
		-c $< \
		-o $@

# Sensor simulator
$(SENSOR_NODE_TARGET): $(SENSOR_NODE_SRC)
	mkdir -p $(BIN_DIR)
	$(CC) \
		$(CPPFLAGS) \
		$(CFLAGS) \
		$< \
		-o $@


# Protocol tests
$(TEST_PROTOCOLS_TARGET): \
	$(TEST_PROTOCOLS_SRC) \
	$(SRC_DIR)/protocols/protocol_adapters.c
	mkdir -p $(BIN_DIR)
	$(CC) \
		$(CPPFLAGS) \
		$(CFLAGS) \
		$(TEST_PROTOCOLS_SRC) \
		$(SRC_DIR)/protocols/protocol_adapters.c \
		-o $@


test-protocols: $(TEST_PROTOCOLS_TARGET)
	./$(TEST_PROTOCOLS_TARGET)

stress:
	$(MAKE) debug
	bash test/stress.sh

# Debug build
debug: CFLAGS += $(DEBUG_FLAGS)
debug: clean all


# ============================================================
# Run
#
# Example:
#   make run ARGS="1234 8080"
# ============================================================

run: all
	./$(TARGET) $(ARGS)


# ============================================================
# Tests:
#   1. Clean debug build
#   2. Protocol unit tests
#   3. Functional test script
# ============================================================

test:
	$(MAKE) debug
	$(MAKE) test-protocols
	bash test/run.sh

# Clean
clean:
	rm -rf $(BUILD_DIR)