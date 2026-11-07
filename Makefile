CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

spectro: spectro.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f spectro

.PHONY: clean
