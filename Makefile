CFLAGS = -Wall -Werror -g -O2 -D_GNU_SOURCE

ALL:	demo_server

COBJS = demo.o scgi.o

demo_server:  $(COBJS)
	gcc $(CFLAGS) -o demo_server $(COBJS)

%.o: %.c
	gcc $(CFLAGS) -c $< -o $@

.PHONY: clean tags

TAGS = GRTAGS GTAGS GPATH GSYMS

clean:
	rm -f $(COBJS) core demo_server $(TAGS)

tags:
	gtags
