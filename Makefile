CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -Wno-unused-result -Wno-format-truncation \
          -Wno-missing-field-initializers -D_GNU_SOURCE
SRCDIR  = core
BUILDDIR = build
TESTDIR = tests
TARGET  = forge

# ── Optional libgit2 support ──────────────────────────────────
# Build with: make WITH_GIT=1
# Without libgit2, the editor works fine — git features (diff
# gutter, blame, timeline) are simply disabled.
ifdef WITH_GIT
  CFLAGS += -DHAS_LIBGIT2
  GIT_LDFLAGS = -lgit2
else
  GIT_LDFLAGS =
endif

SRCS = $(SRCDIR)/arena.c      \
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
       $(SRCDIR)/main.c

OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

# Objects needed for tests (everything except main.o)
TEST_OBJS = $(filter-out $(BUILDDIR)/main.o,$(OBJS))

.PHONY: all clean debug net plugins test

all: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(GIT_LDFLAGS) -ldl

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

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

# ── Debug build ───────────────────────────────────────────────
debug: CFLAGS = -Wall -Wextra -std=c11 -g -DDEBUG -Wno-unused-result -Wno-format-truncation -D_GNU_SOURCE
debug: $(BUILDDIR) $(TARGET)

clean:
	rm -rf $(BUILDDIR) $(TARGET)
	rm -f $(SRCDIR)/*.o
