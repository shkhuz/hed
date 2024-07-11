SRCS := \
	src/main.cpp

OBJS := $(addprefix build/obj/, $(addsuffix .o, $(SRCS)))

INCLUDES := \
	-Ithirdparty/fmt/include \
	-Ithirdparty/libclipboard/include \
	-Ibuild/libclipboard/include
LIBS := -Lbuild/fmt -lfmt -Lbuild/libclipboard/lib -lclipboard -lxcb
FLAGS := -std=c++11 -g -O0 -Wall -Wextra -Wno-unused-parameter -Wno-write-strings
ifdef d
	FLAGS += -D_DEBUG
endif
PREFIX := /usr/local

CC := clang++

run: build/hed
	./build/hed tests/example_cpp.cpp

install: build/hed
	@mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $^ $(DESTDIR)$(PREFIX)/bin/hed
	chmod 755 $(DESTDIR)$(PREFIX)/bin/hed

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/hed

debug: build/hed
	gdb --args ./build/hed tests/example_cpp.cpp

build/hed: $(OBJS) build/fmt/libfmt.a build/libclipboard/lib/libclipboard.a
	$(CC) -o build/hed $(FLAGS) $(OBJS) $(LIBS)

build/fmt/libfmt.a:
	@mkdir -p $(dir $@)
	cd build/fmt; cmake ../../thirdparty/fmt && make fmt

build/libclipboard/lib/libclipboard.a:
	@mkdir -p $(dir $@)
	cd build/libclipboard; cmake ../../thirdparty/libclipboard && make

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

