# make -f desktop.mk -f tools/tests/decoding_floor.mk decoding-floor-tests && build-desktop/decoding-floor-test
decoding-floor-tests: $(BUILD)/decoding-floor-test
$(BUILD)/decoding-floor-test: tools/tests/decoding_floor_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: decoding-floor-tests
