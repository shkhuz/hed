SRCS := \
	thirdparty/fmt/format.cc \
	src/main.cpp

OBJS := $(addprefix build/obj/, $(addsuffix .o, $(SRCS)))

INCLUDES := -Ithirdparty
LIBS := -Lbuild/clip -lclip
FLAGS := -g -O0 -Wall -Wextra -Wno-unused-parameter -Wno-write-strings
ifdef d
	FLAGS += -D_DEBUG
endif

CC := g++

run: build/a.out
	./build/a.out Makefile

debug: build/a.out
	gdb --args ./build/a.out Makefile

build/a.out: $(OBJS) build/clip/libclip.a
	$(CC) -o build/a.out $(FLAGS) $(OBJS) $(LIBS)

build/clip/libclip.a:
	@mkdir -p $(dir $@)
	cd build/clip; cmake ../../thirdparty/clip && make

build/obj/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CC) -c $^ $(FLAGS) -o $@ $(INCLUDES)

build/obj/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	$(CC) -c $^ $(FLAGS) -o $@ $(INCLUDES)

clean:
	rm -rf build/

clean-our:
	rm -rf build/obj/src/main.cpp.o

.PHONY: clean run debug

