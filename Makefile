CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -O2
LDLIBS =

ifeq ($(OS),Windows_NT)
  LDLIBS += -lws2_32 -lshell32
endif

SRCS = src/main.c src/server.c src/game.c src/json.c src/ai.c src/web_assets.c
OBJS = $(SRCS:.c=.o)

all: luansha

luansha: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) luansha

.PHONY: all clean