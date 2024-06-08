SRCS := \
	thirdparty/fmt/format.cc \
	src/main.cpp

OBJS := $(addprefix build/obj/, $(addsuffix .o, $(SRCS)))

INCLUDES := -Ithirdparty
LIBS := 
FLAGS := -g -O0 -Wall -Wextra -Wno-unused-parameter -D_DEBUG

CC := g++

run: $(OBJS) build/a.out
	./build/a.out test_file.txt

debug: $(OBJS) build/a.out
	gdb ./build/a.out

build/a.out: $(OBJS)
	$(CC) -o build/a.out $(FLAGS) $(OBJS) $(LIBS)

build/obj/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CC) -c $^ $(FLAGS) -o $@ $(INCLUDES)

build/obj/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	$(CC) -c $^ $(FLAGS) -o $@ $(INCLUDES)

clean:
	rm -rf build/

.PHONY: clean run debug

