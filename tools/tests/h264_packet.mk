h264-packet-tests: $(BUILD)/h264-packet-test
$(BUILD)/h264-packet-test: tools/tests/h264_packet_test.cpp src/player/h264_packet.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^
.PHONY: h264-packet-tests
