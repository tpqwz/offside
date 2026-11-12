# The offside CLI in spaced C. Emit host C from these files, then compile it.
OFFSIDE ?= ../../c/build/offside
BUILD := build
WARN := -std=c11 -O2 -Wall -Wextra
UNITS := rt engine lang lex host layout pascal spaced main

.PHONY: all clean
all: $(BUILD)/offside

$(BUILD)/offside: $(addprefix $(BUILD)/,$(addsuffix .c,$(UNITS))) $(BUILD)/engine.h $(BUILD)/rt.h
	$(CC) $(WARN) -I$(BUILD) -o $@ $(filter %.c,$^)

$(BUILD)/%.c: %.csp $(OFFSIDE)
	mkdir -p $(BUILD)
	$(OFFSIDE) emit --plain $< > $@

$(BUILD)/%.h: %.h.csp $(OFFSIDE)
	mkdir -p $(BUILD)
	$(OFFSIDE) emit --plain $< > $@

$(OFFSIDE):
	$(MAKE) -C ../../c

clean:
	rm -rf $(BUILD)
