# make -f desktop.mk -f tools/tests/youtube_details.mk youtube-details-tests && build-desktop/youtube-details-test [video id]
youtube-details-tests: $(BUILD)/youtube-details-test
$(BUILD)/youtube-details-test: tools/tests/youtube_details_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: youtube-details-tests
