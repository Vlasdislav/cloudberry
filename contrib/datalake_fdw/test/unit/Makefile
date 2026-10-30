# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

CXX ?= c++
CXXFLAGS ?= -O2 -g
CPPFLAGS += -I../../src
TEST_FLAGS = -std=c++17 -Wall -Wextra -Werror -pthread -UNDEBUG
AWS_SDK_TEST_LIBS ?= -laws-cpp-sdk-core

.PHONY: check check-aws clean

check: sdk_memory_test
	./sdk_memory_test

sdk_memory_test: sdk_memory_test.cpp ../../src/common/sdk_memory.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_FLAGS) $< $(LDFLAGS) -o $@

# Uses a real installed SDK built with CUSTOM_MEMORY_MANAGEMENT=ON.
check-aws:
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_FLAGS) -DDL_TEST_AWS \
		-I$(AWS_SDK_PREFIX)/include sdk_memory_test.cpp \
		-L$(AWS_SDK_PREFIX)/lib64 -L$(AWS_SDK_PREFIX)/lib \
		$(LDFLAGS) $(AWS_SDK_TEST_LIBS) -o sdk_memory_aws_test
	./sdk_memory_aws_test

clean:
	rm -f sdk_memory_test sdk_memory_aws_test
