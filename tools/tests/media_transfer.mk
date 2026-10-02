media-transfer-test: $(BUILD)/media-transfer-test
$(BUILD)/media-transfer-test: tools/tests/media_transfer_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: media-transfer-test
