# Makefile for Atari ST Pong Wars

CC = m68k-atari-mint-gcc
SRC = pongwars.c
TARGET = PONGWARS.TOS
CFLAGS = -O2 -s

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)
