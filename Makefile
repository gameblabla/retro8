PRGNAME     = retro8.elf
CC			= gcc
CXX			= g++

SRCDIR		=  ./src/gen ./src/io ./src/lua ./src/sdl12 ./src/vm ./src/sound
VPATH		= $(SRCDIR)
SRC_C		= $(foreach dir, $(SRCDIR), $(wildcard $(dir)/*.c))
SRC_CP		= $(foreach dir, $(SRCDIR), $(wildcard $(dir)/*.cpp))
OBJ_C		= $(notdir $(patsubst %.c, %.o, $(SRC_C)))
OBJ_CP		= $(notdir $(patsubst %.cpp, %.o, $(SRC_CP)))
OBJS		= $(OBJ_C) $(OBJ_CP)

CFLAGS		= -O0 -g
CFLAGS		+= -DLSB_FIRST -DALIGN_DWORD
CFLAGS		+= -Isrc/gen -Isrc -Isrc/io -Isrc/lua -Isrc/sdl12 -Isrc/vm

ifeq ($(PROFILE), YES)
CFLAGS 		+= -fprofile-generate=/home/retrofw/profile
else ifeq ($(PROFILE), APPLY)
CFLAGS		+= -fprofile-use -fbranch-probabilities
endif
CXXFLAGS = $(CFLAGS)
LDFLAGS     = -lc -lgcc -lm -lSDL -lasound -pthread -no-pie -Wl,--as-needed -Wl,--gc-sections -flto

# Rules to make executable
$(PRGNAME): $(OBJS)  
	$(CXX) $(CFLAGS) -o $(PRGNAME) $^ $(LDFLAGS)

$(OBJ_C) : %.o : %.c
	$(CC) $(CFLAGS) -std=gnu99 -c -o $@ $<

$(OBJ_CP) : %.o : %.cpp
	$(CXX) $(CXXFLAGS) -std=gnu++11 -c -o $@ $<

clean:
	rm -f $(PRGNAME) *.o
