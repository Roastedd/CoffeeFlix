packet-queue-tests: $(BUILD)/packet-queue-test
$(BUILD)/packet-queue-test: tools/tests/packet_queue_test.cpp src/player/packet_queue.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(shell pkg-config --libs libavcodec libavutil)
.PHONY: packet-queue-tests
