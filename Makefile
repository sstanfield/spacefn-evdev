spacefn: spacefn.c
	$(CC) -O3 $(CFLAGS) $< -o $@ $(LDFLAGS)

CFLAGS := `pkg-config --cflags libevdev`
LDFLAGS := `pkg-config --libs libevdev`
