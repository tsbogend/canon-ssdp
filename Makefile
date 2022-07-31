CFLAGS=-Wall -O2 $(shell pkg-config --cflags glib-2.0 gssdp-1.2 gupnp-1.2 gobject-2.0)

LDFLAGS=$(shell pkg-config --libs glib-2.0 gssdp-1.2 gupnp-1.2 gobject-2.0)

canon-ssdp: canon-ssdp.o

clean:
	rm c-ssdp.o c-ssdp

%.o: %.c
	$(CC) $(CFLAGS) $< -c -o $@

%: %.o
	$(CC) $< -o $@ $(LIBS) $(LDFLAGS)
