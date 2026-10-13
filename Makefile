CC := gcc

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


SRC_DIR := src
TOOLS_DIR := tools
TEST_DIR := test

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin


TARGET := $(BIN_DIR)/server

SENSOR_NODE_SRC := $(TOOLS_DIR)/sensor_node.c
SENSOR_NODE_TARGET := $(BIN_DIR)/sensor_node

TEST_PROTOCOLS_SRC := $(TEST_DIR)/test_protocols.c
TEST_PROTOCOLS_TARGET := $(BIN_DIR)/test_protocols


SRC := \
	$(wildcard $(SRC_DIR)/*.c) \
	$(wildcard $(SRC_DIR)/protocols/*.c)


OBJ := \
	$(patsubst \
		$(SRC_DIR)/%.c, \
		$(OBJ_DIR)/%.o, \
		$(SRC))


.PHONY: \
	all \
	debug \
	run \
	clean \
	test \
	test-protocols


all: \
	$(TARGET) \
	$(SENSOR_NODE_TARGET)


$(TARGET): $(OBJ)
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJ) -o $@


$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@


$(SENSOR_NODE_TARGET): $(SENSOR_NODE_SRC)
	mkdir -p $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@


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


debug: CFLAGS += $(DEBUG_FLAGS)
debug: clean all


run: all
	./$(TARGET) $(ARGS)


test:
	$(MAKE) debug
	$(MAKE) test-protocols
	bash test/run.sh


clean:
	rm -rf $(BUILD_DIR)