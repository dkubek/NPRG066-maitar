CC = gcc
CFLAGS = -g -I. -Wall -Werror -Wpedantic

SRCS = mytar.c

OBJS = $(SRCS:.c=.o)

MAIN = mytar

all: $(MAIN)

.PHONY: all

$(MAIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) *.o $(MAIN)
