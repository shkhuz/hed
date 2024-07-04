SRCS := \
	src/main.cpp

OBJS := $(addprefix build/obj/, $(addsuffix .o, $(SRCS)))

INCLUDES := -Ithirdparty/fmt/include
LIBS := -Lbuild/fmt -lfmt
FLAGS := -g -O0 -Wall -Wextra -Wno-unused-parameter -Wno-write-strings
ifdef d
	FLAGS += -D_DEBUG
endif
PREFIX := /usr/local

CC := clang++

run: build/hed
	./build/hed tabtest.txt

install: build/hed
	@mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $^ $(DESTDIR)$(PREFIX)/bin/hed
	chmod 755 $(DESTDIR)$(PREFIX)/bin/hed

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/hed

debug: build/hed
	gdb --args ./build/hed tabtest.txt

build/hed: $(OBJS) build/fmt/libfmt.a
	$(CC) -o build/hed $(FLAGS) $(OBJS) $(LIBS)

build/fmt/libfmt.a:
	@mkdir -p $(dir $@)
	cd build/fmt; cmake ../../thirdparty/fmt && make fmt

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

