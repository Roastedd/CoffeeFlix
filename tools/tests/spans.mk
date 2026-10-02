# make -f desktop.mk -f tools/tests/spans.mk spans-tests
spans-tests: $(BUILD)/spans-test
$(BUILD)/spans-test: tools/tests/spans_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: spans-tests
