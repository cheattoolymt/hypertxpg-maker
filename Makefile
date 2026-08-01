# Makefile for HyperTxPG (htg)
#
# Standard C only (C11), no external libraries. Builds universally with gcc.
# POSIX standard library only; no OS-specific APIs.

CC      ?= gcc
CSTD    ?= -std=c11
CFLAGS  ?= $(CSTD) -Wall -Wextra -O2
LDFLAGS ?=
LDLIBS  ?= -lm

SRC_DIR := src
BUILD   := build
BIN     := htg

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(BIN)
