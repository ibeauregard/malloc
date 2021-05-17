CC = gcc
CFLAGS += -Wall -Wextra -Werror -Wpedantic -g3
SANITIZE = -fsanitize=address
LINKERFLAG = -lm
SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
MAIN = my_malloc

.PHONY = all clean fclean re

all: $(MAIN)

$(MAIN): $(OBJS)
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $(LINKERFLAG) $^

clean:
	$(RM) $(OBJS)

fclean: clean
	$(RM) $(MAIN)

re: fclean all
