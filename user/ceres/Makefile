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
       src/ceres_fixed.c \
       src/ceres_font8x16.c \
       src/ceres_blitter.c \
       src/ceres_sprites.c \
       src/ceres_textures.c \
       src/ceres_terrain.c \
       src/ceres_entities.c \
       src/ceres_sound.c \
       src/ceres_ambient.c \
       src/ceres_cyber.c \
       src/ceres_storage.c \
       src/ceres_game.c \
       src/main_host.c

TEST_SRC := tests/test_game_logic.c \
            src/ceres_fixed.c \
            src/ceres_font8x16.c \
            src/ceres_blitter.c \
            src/ceres_sprites.c \
            src/ceres_textures.c \
            src/ceres_terrain.c \
            src/ceres_entities.c \
            src/ceres_sound.c \
            src/ceres_ambient.c \
            src/ceres_cyber.c \
            src/ceres_storage.c \
            src/ceres_game.c

LOOP_TEST_SRC := tests/test_ceres_loop.c \
                 src/ceres_fixed.c \
                 src/ceres_font8x16.c \
                 src/ceres_blitter.c \
                 src/ceres_sprites.c \
                 src/ceres_textures.c \
                 src/ceres_terrain.c \
                 src/ceres_entities.c \
                 src/ceres_sound.c \
                 src/ceres_ambient.c \
                 src/ceres_cyber.c \
                 src/ceres_storage.c \
                 src/ceres_game.c

OBJ_DIR := build_host
OBJ := $(SRC:src/%.c=$(OBJ_DIR)/%.o)
BIN_DIR := bin
TARGET := $(BIN_DIR)/ceres_host
TEST_TARGET := $(BIN_DIR)/test_game_logic
LOOP_TEST_TARGET := $(BIN_DIR)/test_ceres_loop

.PHONY: all run test clean

all: $(TARGET) $(TEST_TARGET) $(LOOP_TEST_TARGET)

test: $(TARGET) $(TEST_TARGET) $(LOOP_TEST_TARGET)
	@echo "=== Running unit tests (AddressSanitizer + UndefinedBehaviorSanitizer) ==="
	./$(TEST_TARGET)
	@echo "=== Running 6-sector campaign loop test ==="
	./$(LOOP_TEST_TARGET)
	@echo "=== Running headless simulation tests ==="
	SDL_VIDEO_DRIVER=dummy ./$(TARGET) --native --test-frames=60
	SDL_VIDEO_DRIVER=dummy ./$(TARGET) --tab5 --test-frames=30
	SDL_VIDEO_DRIVER=dummy ./$(TARGET) --7b --test-frames=30
	@echo "=== All host test suites passed cleanly ==="

$(TEST_TARGET): $(TEST_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@

$(LOOP_TEST_TARGET): $(LOOP_TEST_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@

$(TARGET): $(OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $^ -o $@ $(SDL_LIBS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

-include $(OBJ:.o=.d)
