CC := gcc

CPPFLAGS := -Iinclude
CFLAGS := -Wall -Wextra -Wpedantic -std=gnu11 -pthread
DEBUG_FLAGS := -g -O0 -DDEBUG

SRC_DIR := src
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin

TARGET := $(BIN_DIR)/server

TOOLS_DIR := tools

SENSOR_NODE_SRC := $(TOOLS_DIR)/sensor_node.c
SENSOR_NODE_TARGET := $(BIN_DIR)/sensor_node

# Current source files + protocol adapters added later
SRCS := $(wildcard $(SRC_DIR)/*.c) \
        $(wildcard $(SRC_DIR)/protocols/*.c)

# src/main.c -> build/obj/main.o
# src/protocols/tbp.c -> build/obj/protocols/tbp.o
OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all debug run clean valgrind helgrind

all: $(TARGET) $(SENSOR_NODE_TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(SENSOR_NODE_TARGET): $(SENSOR_NODE_SRC)
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $< -o $@

debug: CFLAGS += $(DEBUG_FLAGS)
debug: clean all

run: all
	./$(TARGET) $(ARGS)

valgrind: debug
	valgrind --leak-check=full \
	         --show-leak-kinds=all \
	         ./$(TARGET) $(ARGS)

helgrind: debug
	valgrind --tool=helgrind \
	         ./$(TARGET) $(ARGS)

clean:
	rm -rf $(BUILD_DIR)