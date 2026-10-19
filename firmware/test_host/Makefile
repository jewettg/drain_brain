CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -I../components/core/include
test: test_core
	./test_core
test_core: test_core.c ../components/core/db_schedule.c ../components/core/db_bottle.c
	$(CC) $(CFLAGS) -o $@ $^ -lm
clean:
	rm -f test_core
