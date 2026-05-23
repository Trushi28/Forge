CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -Wno-unused-result -Wno-format-truncation -D_GNU_SOURCE
SRCDIR  = core
BUILDDIR = build
TARGET  = forge

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

.PHONY: all clean debug net plugins

all: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lgit2 -ldl

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

net:
	cd net && cargo build

plugins: $(BUILDDIR)
	$(CC) -shared -fPIC -o $(BUILDDIR)/autopairs.so plugins/autopairs.c
	$(CC) -shared -fPIC -o $(BUILDDIR)/indent.so plugins/indent.c
	$(CC) -shared -fPIC -o $(BUILDDIR)/statusbar.so plugins/statusbar.c

debug: CFLAGS = -Wall -Wextra -std=c11 -g -DDEBUG -Wno-unused-result -Wno-format-truncation -D_GNU_SOURCE
debug: $(BUILDDIR) $(TARGET)

clean:
	rm -rf $(BUILDDIR) $(TARGET)
	rm -f $(SRCDIR)/*.o

