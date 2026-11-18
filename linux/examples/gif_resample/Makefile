# Builds the GIF resample test harness. No external dependencies:
# compiles the AnimatedGIF decoder straight from ../../src.
SRC=../../../src
CFLAGS=-O2 -Wall -D__LINUX__ -I$(SRC)

all: gifgen test_resample

gifgen: gifgen.c
	$(CC) -O2 -Wall -o gifgen gifgen.c

test_resample: test_resample.cpp GIFResample.h $(SRC)/AnimatedGIF.cpp $(SRC)/AnimatedGIF.h $(SRC)/gif.inl
	$(CXX) $(CFLAGS) -o test_resample test_resample.cpp $(SRC)/AnimatedGIF.cpp

clean:
	rm -f gifgen test_resample
	rm -rf out
