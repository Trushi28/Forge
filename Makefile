CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -Wno-unused-result -Wno-format-truncation \
          -Wno-missing-field-initializers -D_GNU_SOURCE
SRCDIR  = core
BUILDDIR = build
TESTDIR = tests
TARGET  = forge

# ── Optional libgit2 support ──────────────────────────────────
# Auto-detected with pkg-config (needs the libgit2 *headers*: libgit2-dev /
# libgit2-devel; on Arch just `libgit2`).  Override with:
#   make WITH_GIT=1   force on      make WITH_GIT=0   force off
# Without libgit2 the editor works fine — git features (diff gutter, blame,
# timeline) are simply disabled.
ifeq ($(origin WITH_GIT),undefined)
  ifneq ($(shell pkg-config --exists libgit2 2>/dev/null && echo yes),)
    WITH_GIT = 1
  endif
endif
ifeq ($(WITH_GIT),1)
  CFLAGS += -DHAS_LIBGIT2 $(shell pkg-config --cflags libgit2 2>/dev/null)
  GIT_LDFLAGS = $(or $(shell pkg-config --libs libgit2 2>/dev/null),-lgit2)
else
  GIT_LDFLAGS =
endif

SRCS = $(SRCDIR)/arena.c      \
       $(SRCDIR)/utf8.c       \
       $(SRCDIR)/buffer.c     \
       $(SRCDIR)/config.c     \
       $(SRCDIR)/theme.c      \
       $(SRCDIR)/input.c      \
       $(SRCDIR)/ui.c         \
       $(SRCDIR)/render.c     \
       $(SRCDIR)/lsp.c        \
       $(SRCDIR)/completion.c \
       $(SRCDIR)/palette.c    \
       $(SRCDIR)/undo.c       \
       $(SRCDIR)/git.c        \
       $(SRCDIR)/plugin.c     \
       $(SRCDIR)/forgescript.c \
       $(SRCDIR)/ipc.c        \
       $(SRCDIR)/globals.c    \
       $(SRCDIR)/main.c

OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

# Objects needed for tests (everything except main.o)
TEST_OBJS = $(filter-out $(BUILDDIR)/main.o,$(OBJS))

.PHONY: all clean debug net plugins test fuzz gittest FORCE

all: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(GIT_LDFLAGS) -ldl

# Rebuild every object when the compiler flags change (e.g. libgit2 toggled).
# Without this, `make` after installing libgit2 said "Nothing to be done" and
# kept the no-op git.o stub.  Header dependencies are tracked with -MMD, so an
# edited .h (e.g. buffer.h's struct) also rebuilds its users.
$(BUILDDIR)/.flags: FORCE | $(BUILDDIR)
	@echo '$(CC) $(CFLAGS) $(GIT_LDFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS) $(GIT_LDFLAGS)' > $@

FORCE:

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(BUILDDIR)/.flags | $(BUILDDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

# ── Rust net crate ────────────────────────────────────────────
net:
	cd net && cargo build

# ── Plugins ───────────────────────────────────────────────────
plugins: $(BUILDDIR)
	$(CC) -shared -fPIC -o $(BUILDDIR)/autopairs.so plugins/autopairs.c
	$(CC) -shared -fPIC -o $(BUILDDIR)/indent.so plugins/indent.c
	$(CC) -shared -fPIC -o $(BUILDDIR)/statusbar.so plugins/statusbar.c

# ── Test suite ────────────────────────────────────────────────
test: $(BUILDDIR) $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $(BUILDDIR)/test_forge $(TESTDIR)/test_main.c $(TEST_OBJS) $(GIT_LDFLAGS) -ldl
	@echo "════════════════════════════════════════════════"
	@echo "  Running Forge C core tests"
	@echo "════════════════════════════════════════════════"
	@$(BUILDDIR)/test_forge

# ── Sanitizer tests: piece-tree differential fuzz, IPC framing, LSP JSON parser
SAN = -std=c11 -D_GNU_SOURCE -g -fsanitize=address,undefined
fuzz: $(BUILDDIR)
	$(CC) $(SAN) -o $(BUILDDIR)/fuzz $(TESTDIR)/test_buffer_fuzz.c \
	    $(SRCDIR)/buffer.c $(SRCDIR)/arena.c $(SRCDIR)/globals.c
	$(BUILDDIR)/fuzz
	$(CC) $(SAN) -o $(BUILDDIR)/ipc_test $(TESTDIR)/test_ipc.c $(SRCDIR)/ipc.c -lpthread
	$(BUILDDIR)/ipc_test
	$(CC) $(SAN) -o $(BUILDDIR)/lsp_json_test $(TESTDIR)/test_lsp_json.c
	$(BUILDDIR)/lsp_json_test
	$(CC) $(SAN) -o $(BUILDDIR)/palette_test $(TESTDIR)/test_palette_render.c \
	    $(SRCDIR)/palette.c $(SRCDIR)/theme.c
	$(BUILDDIR)/palette_test

# ── libgit2 integration test (skips itself when libgit2 is off)
gittest: $(BUILDDIR)
	$(CC) $(CFLAGS) -o $(BUILDDIR)/git_test $(TESTDIR)/test_git.c $(SRCDIR)/git.c $(GIT_LDFLAGS)
	$(BUILDDIR)/git_test

# ── Debug build ───────────────────────────────────────────────
debug: CFLAGS = -Wall -Wextra -std=c11 -g -DDEBUG -Wno-unused-result -Wno-format-truncation -D_GNU_SOURCE
debug: $(BUILDDIR) $(TARGET)

clean:
	rm -rf $(BUILDDIR) $(TARGET)
	rm -f $(SRCDIR)/*.o
