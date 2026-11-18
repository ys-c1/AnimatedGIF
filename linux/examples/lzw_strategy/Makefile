CXX ?= g++
CXXFLAGS = -O2 -Wall -D__LINUX__ -I../../../src

all: lzw_strategy

lzw_strategy: lzw_strategy.cpp ../../../src/AnimatedGIF.cpp ../../../src/AnimatedGIF.h ../../../src/gif.inl
	$(CXX) $(CXXFLAGS) lzw_strategy.cpp ../../../src/AnimatedGIF.cpp -o lzw_strategy

run: lzw_strategy
	./lzw_strategy

clean:
	rm -f lzw_strategy sample_*.gif
