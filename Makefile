SRC_DIR = src
BUILD_DIR = build
BIN_DIR = build/bin
INT_DIR = build/int
CC = gcc
CFLAGS = -Wall -Wextra
INPUT_FILE = test/test.jot

ifeq ($(OS),Windows_NT)
	EXT = .exe
	CLEAR = cls
	CLEAN = if exist "$(BUILD_DIR)" rmdir /S /Q "$(BUILD_DIR)"
	MKDIR = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
	RUN_CMD = $(TARGET)
	DEL_OBJS = if exist "$(subst /,\,$(INT_DIR))" del /Q $(subst /,\,$(OBJS))
	DEL_TARGET = if exist "$(subst /,\,$(TARGET))" del "$(subst /,\,$(TARGET))"
else
	EXT =
	CLEAR = clear
	CLEAN = rm -rf $(BUILD_DIR)
	MKDIR = mkdir -p "$(1)"
	RUN_CMD = ./$(TARGET)
	DEL_OBJS = rm -f $(OBJS)
	DEL_TARGET = rm -f $(TARGET)
endif

rwildcard = $(wildcard $1$2) $(foreach d,$(wildcard $1*/),$(call rwildcard,$d,$2))
SRCS = $(call rwildcard,$(SRC_DIR)/,*.c)

TARGET = $(BIN_DIR)/jotc$(EXT)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(INT_DIR)/%.o,$(SRCS))
OBJ_DIRS = $(sort $(dir $(OBJS)))

.PHONY: all clear clean build link run help

help:
	@echo all - clear, build, link, run, then delete the binary
	@echo build - compile every .c under $(SRC_DIR) into $(INT_DIR)
	@echo link - link object files into $(TARGET)
	@echo run - execute $(TARGET)
	@echo clean - delete $(BUILD_DIR)
	@echo help - show this message

all: clear clean build link run
	@$(CLEAN)

clear:
	@$(CLEAR)

clean:
	@$(CLEAN)

build:
	@$(DEL_OBJS)
	$(MAKE) $(OBJS)

link: | $(BIN_DIR)
	@$(DEL_TARGET)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET)

run:
	@$(RUN_CMD) $(INPUT_FILE)

$(OBJS): | $(OBJ_DIRS)

$(OBJ_DIRS) $(BIN_DIR):
	@$(call MKDIR,$@)

$(INT_DIR)/%.o: $(SRC_DIR)/%.c
	@echo $< -> $@
	$(CC) $(CFLAGS) -c $< -o $@
