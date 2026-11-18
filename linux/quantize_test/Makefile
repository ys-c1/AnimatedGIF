# GIF RGB888 quantizer acceptance test
# Single translation unit: the test includes gif.inl (which itself includes
# AnimatedGIF.h), so no other library object is needed. No external libs
# beyond libc/libm, both part of the toolchain.
CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -D__LINUX__ -I../../src
LDLIBS = -lm

all: quantize_test

quantize_test: quantize_test.cpp ../../src/gif.inl ../../src/AnimatedGIF.h
	$(CXX) $(CXXFLAGS) quantize_test.cpp -o quantize_test $(LDLIBS)

test: quantize_test
	./quantize_test

clean:
	rm -f quantize_test

.PHONY: all test clean
