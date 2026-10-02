# make -f desktop.mk -f tools/tests/blackbox.mk blackbox-tests && build-desktop/blackbox-test
blackbox-tests: $(BUILD)/blackbox-test
$(BUILD)/blackbox-test: tools/tests/blackbox_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: blackbox-tests
