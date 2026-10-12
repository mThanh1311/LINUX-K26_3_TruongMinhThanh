CC := gcc

CFLAGS := -Wall -Wextra -pthread -Iinclude
DEBUG_FLAGS := -g -O0 -DDEBUG

SRC_DIR := src
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin

TARGET := $(BIN_DIR)/sensor_gateway

.PHONY: all debug run clean

all: $(TARGET)

$(TARGET): $(SRC_DIR)/main.c
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $< -o $@

debug: CFLAGS += $(DEBUG_FLAGS)
debug: clean all

run: all
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR)