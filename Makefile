CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

ifeq ($(OS),Windows_NT)
# MinGW-w64: build with mingw32-make (make's default CC "cc" doesn't exist there)
CC = gcc
TARGET = predator_i2ec.exe
LDLIBS = -lole32 -loleaut32 -lwbemuuid
else
TARGET = predator_i2ec
endif
SRC = predator_i2ec.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
