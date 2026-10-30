# Native offside. The warning set is the CERT C build.
# Extra CFLAGS from the environment are appended (an -arch flag must not wipe these).
WARN := -std=c11 -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wformat-security \
	-Wnull-dereference -Wstrict-prototypes -Wshadow -Wundef \
	-fstack-protector-strong -D_FORTIFY_SOURCE=2
CFLAGS ?=
CPPFLAGS ?= -Isrc
LDFLAGS ?=

SRC := src/rt.c src/engine.c src/lang.c src/lex.c src/host.c src/layout.c src/pascal.c src/spaced.c src/main.c
BUILD := build
OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))

.PHONY: all clean
all: $(BUILD)/offside

$(BUILD)/offside: $(OBJ)
	$(CC) $(WARN) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

$(BUILD)/%.o: src/%.c src/engine.h src/rt.h
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(WARN) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)
