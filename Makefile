CXX      ?= g++
CXXFLAGS ?= -std=c++14 -O2 -pthread -Wall -Wextra

TARGET = reservation_system
SRCS   = main.cpp helper.cpp shared_table.cpp
OBJS   = $(SRCS:.cpp=.o)
HDRS   = shared_table.h helper.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

bench: $(TARGET)
	./bench.sh

clean:
	rm -f $(OBJS) $(TARGET) results.csv

.PHONY: all bench clean
