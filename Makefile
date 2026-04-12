CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -DFLIV_VERSION=\"$(shell cat VERSION)\" $(shell fltk-config --use-gl --cxxflags) $(shell pkg-config --cflags imlib2 libmagic)
LDFLAGS := $(shell fltk-config --use-gl --ldflags) $(shell pkg-config --libs imlib2 libmagic)

TARGET := fliv
SRCS := main.cpp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
