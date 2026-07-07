ifeq ($(OS),Windows_NT)
    detected_OS := Windows
else
    detected_OS := $(shell uname -s)
endif

ifeq ($(detected_OS),Windows)
    CC ?= gcc
	RM_CMD = del
    EXE = .exe
    RUN = 
else ifeq ($(detected_OS),Darwin)
    CC ?= gcc
	RM_CMD = rm
    EXE =
    RUN = ./
else
    CC ?= gcc
	RM_CMD = rm
    EXE =
    RUN = ./
endif

all: sploot$(EXE)

sploot$(EXE): src/sploot.c
	$(CC) src/lib.c -o temp$(EXE)
	$(RUN)temp$(EXE)
	$(RM_CMD) temp$(EXE)
	$(CC) -O2 src/sploot.c -o sploot$(EXE) -lm -lz