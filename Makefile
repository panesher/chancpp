BOOST_DIR=/opt/homebrew/opt/boost

test:
	clang++ -std=c++20 -O3 -fsanitize=thread -pthread chan_tests.cpp -o chan_tests.out \
		-I$(BOOST_DIR)/include -L$(BOOST_DIR)/lib
	./chan_tests.out

test_stress:
	clang++ -std=c++20 -O3 -fsanitize=thread -pthread chan_stress_tests.cpp -o chan_stress_tests.out \
		-I$(BOOST_DIR)/include -L$(BOOST_DIR)/lib
	./chan_stress_tests.out
