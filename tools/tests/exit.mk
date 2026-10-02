# make -f desktop.mk -f tools/tests/exit.mk exit-tests   (builds; tools/tests/exit.py runs it)
exit-tests: $(BUILD)/exit-test
$(BUILD)/exit-test: tools/tests/exit_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: exit-tests
