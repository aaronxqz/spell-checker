CC = gcc
CFLAGS = -g -std=c99 -Wall -fsanitize=address,undefined

all: spell.o spell spelltest

spell.o: spell.h spell.c
	$(CC) $(CFLAGS) -c spell.c

spell: spell.h spell.c
	$(CC) $(CFLAGS) -o spell spell.c

spelltest: spelltest.c
	$(CC) $(CFLAGS) -o spelltest spelltest.c

clean:
	rm -f *.o spelltest spell spell.o
