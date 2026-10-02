# make -f desktop.mk -f tools/tests/heartbeat.mk heartbeat-tests && build-desktop/heartbeat-test
heartbeat-tests: $(BUILD)/heartbeat-test
$(BUILD)/heartbeat-test: tools/tests/heartbeat_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: heartbeat-tests
