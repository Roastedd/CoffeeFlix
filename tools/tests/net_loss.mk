# make -f desktop.mk -f tools/tests/net_loss.mk net-loss-tests
net-loss-tests: $(BUILD)/net-loss-test
$(BUILD)/net-loss-test: tools/tests/net_loss_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: net-loss-tests
