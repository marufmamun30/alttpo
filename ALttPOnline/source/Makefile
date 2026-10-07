# A Link to the Past Online
# Windows (MSYS2 MinGW64):  make            -> alttpo.exe, SDL2 linked statically
# Linux / macOS:            make            -> alttpo (needs SDL2 + zlib dev packages; randomizer launching is Windows only)

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wno-unused-function
SRCS := $(wildcard src/*.c) $(wildcard src/snes/*.c)
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

ifeq ($(OS),Windows_NT)
  TARGET := alttpo.exe
  SDLFLAGS := $(shell sdl2-config --cflags)
  LIBS := -static $(shell sdl2-config --static-libs) -lz -lws2_32 -liphlpapi
  RES := build/res.o
else
  TARGET := alttpo
  SDLFLAGS := $(shell sdl2-config --cflags)
  LIBS := $(shell sdl2-config --libs) -lz -lm
  RES :=
endif

all: $(TARGET)

$(TARGET): $(OBJS) $(RES)
	$(CC) -o $@ $^ $(LIBS)

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDLFLAGS) -MMD -MP -c $< -o $@

build/res.o: res/alttpo.rc res/alttpo.ico
	windres $< -O coff -o $@

clean:
	rm -rf build $(TARGET)

-include $(DEPS)

.PHONY: all clean
