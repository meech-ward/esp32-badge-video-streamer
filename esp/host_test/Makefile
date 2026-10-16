# Host checks for the wire protocol and software AWB, without ESP-IDF.
CXX ?= c++
PYTHON ?= python3
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Werror
# Set SANITIZERS= for compilers or hosts without sanitizer support.
SANITIZERS ?= -fsanitize=address,undefined
INC = -I../components/protocol/include -I../components/camera_source/include -Ibuild
BUILD = build

all: run

$(BUILD)/vectors.h: gen_vectors.py ../../tufty/video_streamer/stream_protocol.py
	mkdir -p $(BUILD)
	$(PYTHON) gen_vectors.py $@

$(BUILD)/test_protocol: test_protocol.cpp $(BUILD)/vectors.h ../components/protocol/include/streamer/protocol.hpp
	$(CXX) $(CXXFLAGS) $(SANITIZERS) $(INC) test_protocol.cpp -o $@

$(BUILD)/test_awb: test_awb.cpp ../components/camera_source/include/streamer/white_balance.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(SANITIZERS) $(INC) test_awb.cpp -o $@

run: $(BUILD)/test_protocol $(BUILD)/test_awb
	./$(BUILD)/test_protocol
	./$(BUILD)/test_awb

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
