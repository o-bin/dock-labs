# Native Termux/Linux Makefile
CC = clang
CFLAGS = -O3 -I.
LDFLAGS = -lpthread

TARGET = memory_server

all: $(TARGET)

$(TARGET): memory_server.c lz4.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET)
