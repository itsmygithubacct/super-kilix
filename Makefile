CC      ?= cc
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk
override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	$(KILIX_GAME_KIT_CPPFLAGS)
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
# Byte-reproducibility: forbid fused multiply-add so float results are identical
# across builds and machines (--selftest must reproduce exactly).
override CFLAGS += -ffp-contract=off
LDFLAGS ?=
LDLIBS  ?= $(KILIX_GAME_KIT_LDLIBS)
PREFIX  ?= /usr/local
DESTDIR ?=

SRC = src/main.c src/game.c src/data.c src/render.c src/term.c src/sound.c
OBJ = $(SRC:.c=.o)
BIN = super-kilix

# The included game-kit.mk defines the library archive rule first, which would
# otherwise capture the default goal; keep the game binary as the default.
.DEFAULT_GOAL := all

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

src/%.o: src/%.c src/super_kilix.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

test: $(BIN) clean-room-check test-cli
	./$(BIN) --rules-test
	./$(BIN) --input-test
	./$(BIN) --sound-test
	./$(BIN) --selftest 1337 12000
	./$(BIN) --selftest 42 6000
	@a=$$(mktemp); b=$$(mktemp); \
	trap 'rm -f "$$a" "$$b"' EXIT HUP INT TERM; \
	./$(BIN) --selftest 1337 6000 >"$$a"; \
	./$(BIN) --selftest 1337 6000 >"$$b"; \
	cmp -s "$$a" "$$b" || { echo "selftest is not byte-deterministic" >&2; exit 1; }
	@set -eu; \
	render_dir=$$(mktemp -d); \
	trap 'rm -rf "$$render_dir"' EXIT HUP INT TERM; \
	SUPER_KILIX_RENDER_DIR="$$render_dir" ./$(BIN) --render-test 7; \
	set -- "$$render_dir"/render_*.ppm; \
	[ "$$#" -eq 15 ] || { echo "expected 15 render fixtures, found $$#" >&2; exit 1; }; \
	for image do \
		[ -s "$$image" ] || { echo "empty render fixture: $$image" >&2; exit 1; }; \
		header=$$(head -n 3 "$$image"); set -- $$header; \
		[ "$$#" -eq 4 ] && [ "$$1" = P6 ] && [ "$$4" = 255 ] || \
			{ echo "invalid PPM header: $$image" >&2; exit 1; }; \
		width=$$2; height=$$3; \
		header_bytes=$$(printf 'P6\n%s %s\n255\n' "$$width" "$$height" | wc -c); \
		expected=$$((header_bytes + width * height * 3)); \
		actual=$$(wc -c < "$$image"); \
		[ "$$actual" -eq "$$expected" ] || \
			{ echo "invalid PPM payload: $$image ($$actual, expected $$expected)" >&2; exit 1; }; \
		tail -c +$$((header_bytes + 1)) "$$image" | od -An -tu1 | \
			awk '{ for (i=1; i<=NF; i++) if (!seen[$$i]++) colors++ } \
			     END { exit colors < 16 }' || \
			{ echo "render fixture is effectively blank: $$image" >&2; exit 1; }; \
	done; \
	! cmp -s "$$render_dir/render_walk_stride_a.ppm" \
	          "$$render_dir/render_walk_stride_b.ppm" || \
		{ echo "walk strides render identically" >&2; exit 1; }

test-fast: $(BIN) test-cli
	./$(BIN) --rules-test
	./$(BIN) --input-test
	./$(BIN) --selftest 1337 1800

test-cli: $(BIN)
	@./$(BIN) --version >/dev/null
	@./$(BIN) --dump-level 1 >/dev/null
	@status=0; ./$(BIN) --dump-level 1junk >/dev/null 2>&1 || status=$$?; \
		[ "$$status" -eq 2 ]
	@status=0; ./$(BIN) --level +1 >/dev/null 2>&1 || status=$$?; \
		[ "$$status" -eq 2 ]
	@status=0; ./$(BIN) --selftest 1 2 extra >/dev/null 2>&1 || status=$$?; \
		[ "$$status" -eq 2 ]

# Clean-room guard: an original platformer that leaks no reference-game asset
# files and names nothing forbidden anywhere in the shipped tree.
clean-room-check:
	@bad=$$(find . \( -path './.git' -o -path './third_party' \) -prune -o \
		-type f \( -iname '*.nes' -o -iname '*.nsf' -o -iname '*.chr' \
		-o -iname '*.prg' -o -iname '*.fm2' -o -iname '*.bk2' \
		-o -iname '*.pal' \) -print); \
		[ -z "$$bad" ] || { echo "reference-format files found:" >&2; \
		echo "$$bad" >&2; exit 1; }
	@! grep -R -i -E \
		'mario|luigi|goomba|koopa|bowser|nintendo|toad|peach|piranha|lakitu|buzzy|hammer *bro|super *mario|smb1?' \
		src docs README.md
	@! grep -R -i -E '[c]laude|[a]nthropic' src docs README.md

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -Wall -Wextra -Wpedantic -std=c11 -ffp-contract=off -fsanitize=address,undefined -fno-omit-frame-pointer' \
		LDFLAGS='-fsanitize=address,undefined'
	./$(BIN) --rules-test
	./$(BIN) --input-test
	./$(BIN) --selftest 9 2400
	@render_dir=$$(mktemp -d); \
	trap 'rm -rf "$$render_dir"' EXIT HUP INT TERM; \
	SUPER_KILIX_RENDER_DIR="$$render_dir" ./$(BIN) --render-test 9
	$(MAKE) clean
	$(MAKE)

install: $(BIN)
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	install -Dm644 docs/super-kilix.6 "$(DESTDIR)$(PREFIX)/share/man/man6/super-kilix.6"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	rm -f "$(DESTDIR)$(PREFIX)/share/man/man6/super-kilix.6"

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(BIN) render_*.ppm render_*.png

-include $(OBJ:.o=.d)

.PHONY: all test test-fast test-cli clean-room-check sanitize install uninstall clean
