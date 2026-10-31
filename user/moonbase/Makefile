CC ?= clang
CFLAGS ?= -Wall -Wextra -std=c11 -O2 -g
CFLAGS += -Iinclude -MMD -MP

# AddressSanitizer & UndefinedBehaviorSanitizer enabled by default for host safety
ifeq ($(ASAN), 0)
    SAN_FLAGS :=
else
    SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
endif

CFLAGS += $(SAN_FLAGS)

# Host SDL3 dependency
SDL_CFLAGS := $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl3 2>/dev/null || echo "-lSDL3")

SRC := src/platform_sdl3.c \
       src/moon_fixed.c \
       src/moon_font8x16.c \
       src/moon_blitter.c \
       src/moon_grid.c \
       src/moon_sprites.c \
       src/moon_textures.c \
       src/moon_structures.c \
       src/moon_resources.c \
       src/moon_starship.c \
       src/moon_ui.c \
       src/moon_ambient.c \
       src/moon_cyber.c \
       src/moon_bench.c \
       src/moon_game.c \
       src/main_host.c

TEST_SRC := tests/test_fixed_blitter.c \
            src/moon_fixed.c \
            src/moon_font8x16.c \
            src/moon_blitter.c \
            src/moon_grid.c \
            src/moon_sprites.c \
            src/moon_textures.c \
            src/moon_structures.c \
            src/moon_resources.c \
            src/moon_starship.c \
            src/moon_ambient.c \
            src/moon_cyber.c \
            src/moon_ui.c

OBJ_DIR := build_host
OBJ := $(SRC:src/%.c=$(OBJ_DIR)/%.o)
BIN_DIR := bin
TARGET := $(BIN_DIR)/moonbase_host
TEST_TARGET := $(BIN_DIR)/test_fixed_blitter
LOGIC_TARGET := $(BIN_DIR)/test_game_logic
BALANCE_TARGET := $(BIN_DIR)/balance
LOGIC_SRC := tests/test_game_logic.c $(filter-out tests/test_fixed_blitter.c,$(TEST_SRC)) src/moon_game.c

.PHONY: all run run_tab5 test balance clean

all: $(TARGET) $(TEST_TARGET) $(LOGIC_TARGET) $(BALANCE_TARGET)

test: $(TEST_TARGET) $(LOGIC_TARGET) $(BALANCE_TARGET) $(TARGET)
	./$(TEST_TARGET)
	./$(LOGIC_TARGET)
	./$(BALANCE_TARGET) check
	SDL_VIDEO_DRIVER=dummy ./$(TARGET) --7b --test-frames=15
	SDL_VIDEO_DRIVER=dummy ./$(TARGET) --tab5 --test-frames=15

$(TEST_TARGET): $(TEST_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@

$(LOGIC_TARGET): $(LOGIC_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@

# Balance runs: `make balance` prints the per-level table, `bin/balance sweep` / `one` / `seeds` explore
BALANCE_SRC := tests/balance.c $(filter-out tests/test_fixed_blitter.c,$(TEST_SRC)) src/moon_game.c
$(BALANCE_TARGET): $(BALANCE_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@

balance: $(BALANCE_TARGET)
	./$(BALANCE_TARGET) levels 16

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

$(TARGET): $(OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ $(SDL_LIBS) -o $@

run: $(TARGET)
	./$(TARGET)

run_tab5: $(TARGET)
	./$(TARGET) --tab5

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

-include $(OBJ:.o=.d)
