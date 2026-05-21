CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -Wno-unused-result
SRCDIR  = core
TARGET  = forge

SRCS = $(SRCDIR)/arena.c   \
       $(SRCDIR)/buffer.c  \
       $(SRCDIR)/input.c   \
       $(SRCDIR)/ui.c      \
       $(SRCDIR)/render.c  \
       $(SRCDIR)/main.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean debug

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

debug: CFLAGS = -Wall -Wextra -std=c11 -g -DDEBUG -Wno-unused-result
debug: $(TARGET)

clean:
	rm -f $(SRCDIR)/*.o $(TARGET)
