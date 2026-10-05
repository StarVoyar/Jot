SRC_DIR = src
BUILD_DIR = build
BIN_DIR = build/bin
INT_DIR = build/int
GENERATED_DIR = build/bin/generated
CC = gcc
CFLAGS = -Wall -Wextra
NASM = nasm
INPUT_FILE = src/main.jot

ifeq ($(OS),Windows_NT)
	EXT = .exe
	CLEAR = cls
	CLEAN = if exist "$(BUILD_DIR)" rmdir /S /Q "$(BUILD_DIR)"
	MKDIR = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
	RUN_CMD = $(subst /,\,$(TARGET))
	RUN_EXE = $(subst /,\,$(GENERATED_EXE))
	NASM_FMT = win64
	DEL_OBJS = if exist "$(subst /,\,$(INT_DIR))" del /Q $(subst /,\,$(OBJS))
	DEL_TARGET = if exist "$(subst /,\,$(TARGET))" del "$(subst /,\,$(TARGET))"
	CLEAN_ON_FAIL = || (if exist "$(BUILD_DIR)" rmdir /S /Q "$(BUILD_DIR)" & exit 1)
else
	EXT =
	CLEAR = clear
	CLEAN = rm -rf $(BUILD_DIR)
	MKDIR = mkdir -p "$(1)"
	RUN_CMD = ./$(TARGET)
	RUN_EXE = ./$(GENERATED_EXE)
	NASM_FMT = elf64
	DEL_OBJS = rm -f $(OBJS)
	DEL_TARGET = rm -f $(TARGET)
	CLEAN_ON_FAIL = || ($(CLEAN); exit 1)
endif

rwildcard = $(wildcard $1$2) $(foreach d,$(wildcard $1*/),$(call rwildcard,$d,$2))
SRCS = $(call rwildcard,$(SRC_DIR)/,*.c)

TARGET = $(BIN_DIR)/jotc$(EXT)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(INT_DIR)/%.o,$(SRCS))
OBJ_DIRS = $(sort $(dir $(OBJS)))

ASM_BASE = $(basename $(notdir $(INPUT_FILE)))
GENERATED_ASM = $(GENERATED_DIR)/$(ASM_BASE).asm
GENERATED_OBJ = $(GENERATED_DIR)/$(ASM_BASE).obj
GENERATED_EXE = $(GENERATED_DIR)/$(ASM_BASE)$(EXT)

.PHONY: all clear clean build link run debug help

help:
	@echo all - clear, clean, build, link, debug, run, clean
	@echo build - compile every .c under $(SRC_DIR) into $(INT_DIR)
	@echo link - link object files into $(TARGET)
	@echo debug - lex, parse and codegen $(INPUT_FILE) with tokens, AST and asm path printed
	@echo run - compile $(INPUT_FILE) to asm, assemble, link and execute the program
	@echo clean - delete $(BUILD_DIR)
	@echo help - show this message

all: clear clean build link
	@$(MAKE) debug
	@$(MAKE) run
	@$(MAKE) clean

clear:
	@$(CLEAR)

clean:
	@$(CLEAN)

build:
	@$(DEL_OBJS)
	$(MAKE) $(OBJS)

link: | $(BIN_DIR) $(GENERATED_DIR)
	@$(DEL_TARGET)
	$(if $(QUIET),@)$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(CLEAN_ON_FAIL)

$(TARGET): $(OBJS) | $(BIN_DIR) $(GENERATED_DIR)
	@$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(CLEAN_ON_FAIL)

debug: build link | $(GENERATED_DIR)
	@$(RUN_CMD) $(INPUT_FILE) $(GENERATED_ASM) --debug $(CLEAN_ON_FAIL)

run: QUIET = 1
run: $(TARGET) | $(GENERATED_DIR)
	@$(RUN_CMD) $(INPUT_FILE) $(GENERATED_ASM) $(CLEAN_ON_FAIL)
	@$(NASM) -f $(NASM_FMT) $(GENERATED_ASM) -o $(GENERATED_OBJ) $(CLEAN_ON_FAIL)
	@$(CC) $(GENERATED_OBJ) -o $(GENERATED_EXE) $(CLEAN_ON_FAIL)
	-@$(RUN_EXE)

$(OBJS): | $(OBJ_DIRS)

$(OBJ_DIRS) $(BIN_DIR) $(GENERATED_DIR):
	@$(call MKDIR,$@)

$(INT_DIR)/%.o: $(SRC_DIR)/%.c
	$(if $(QUIET),,@echo $< : $@)
	$(if $(QUIET),@)$(CC) $(CFLAGS) -c $< -o $@ $(CLEAN_ON_FAIL)
