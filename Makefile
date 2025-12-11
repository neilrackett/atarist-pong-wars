# Makefile for Atari ST Pong Wars
# Uses libcmini to reduce file size from 132KB to 6KB

CC := m68k-atari-mint-gcc
LIBCMINI := /freemint/libcmini/lib
CFLAGS := -O2 -s -std=gnu99 -I/freemint/libcmini/include -nostdlib $(LIBCMINI)/crt0.o
LDFLAGS := -s -L$(LIBCMINI) -lcmini -lgcc

SRC_DIR := src
BUILD_DIR := build
AUTO_DIR := $(BUILD_DIR)/AUTO

PONGWARS_TARGET := PONGWARS.TOS
PONGWARS_SPLASH := PONGWARS.PI1
PONGWARS_SRC := pongwars.c

LOADER_TARGET := LOADER.PRG
LOADER_SRC := loader.c

# Rules

all: $(PONGWARS_TARGET) $(LOADER_TARGET)

$(PONGWARS_TARGET):
	@echo "Building $@..."
	mkdir -p $(BUILD_DIR)
	cp $(SRC_DIR)/$(PONGWARS_SPLASH) $(BUILD_DIR)/$(PONGWARS_SPLASH)
	$(CC) $(CFLAGS) $(SRC_DIR)/$(PONGWARS_SRC) -o $(BUILD_DIR)/$(PONGWARS_TARGET) $(LDFLAGS)

$(LOADER_TARGET):
	@echo "Building $@..."
	mkdir -p $(AUTO_DIR)
	$(CC) $(CFLAGS) $(SRC_DIR)/$(LOADER_SRC) -o $(AUTO_DIR)/$(LOADER_TARGET) $(LDFLAGS)

clean:
	@echo "Cleaning..."
	rm -rf $(BUILD_DIR)/*

.PHONY: all clean