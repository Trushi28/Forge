CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -Wno-unused-result -Wno-format-truncation -D_GNU_SOURCE
SRCDIR  = core
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
       $(SRCDIR)/main.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean debug

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

debug: CFLAGS = -Wall -Wextra -std=c11 -g -DDEBUG -Wno-unused-result -Wno-format-truncation -D_GNU_SOURCE
debug: $(TARGET)

clean:
	rm -f $(SRCDIR)/*.o $(TARGET)
