# make -f desktop.mk -f tools/tests/player_end.mk player-end-tests
player-end-tests: $(BUILD)/player-end-test
$(BUILD)/player-end-test: tools/tests/player_end_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: player-end-tests
