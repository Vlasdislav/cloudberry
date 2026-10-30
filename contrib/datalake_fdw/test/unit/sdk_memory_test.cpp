/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <array>
#include <cstdio>
#include <thread>
#include <vector>

#include "common/sdk_memory.h"

#ifdef DL_TEST_AWS
#include "common/aws_memory_manager.h"
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/common/allocator.h>
#endif

static void
alignment_and_lifetime()
{
	DlSdkMemoryLedger ledger;
	for (std::size_t alignment : {0, 1, 8, 16, 64, 4096})
	{
		void *pointer = ledger.Allocate(1003, alignment);
		assert(pointer != nullptr);
		assert(reinterpret_cast<std::uintptr_t>(pointer) %
			   std::max(alignment, alignof(std::max_align_t)) == 0);
		std::memset(pointer, 0x7a, 1003);
		auto allocated = ledger.TakeSnapshot();
		assert(allocated.live > 1003);
		assert(allocated.peak == allocated.live);
		ledger.Free(pointer);
		auto freed = ledger.TakeSnapshot();
		assert(freed.live == 0);
		assert(freed.peak == allocated.live);
		assert(ledger.TakeSnapshot().peak == 0);
	}
	ledger.Free(nullptr);
	void *zero = ledger.Allocate(0, 0);
	assert(zero != nullptr);
	ledger.Free(zero);
	assert(ledger.TakeSnapshot().live == 0);
}

static void
invalid_requests()
{
	DlSdkMemoryLedger ledger;
	const auto maximum = std::numeric_limits<std::size_t>::max();
	assert(ledger.Allocate(maximum, 16) == nullptr);
	assert(ledger.Allocate(10, maximum) == nullptr);
	assert(ledger.Allocate(10, 65) == nullptr);
	assert(ledger.TakeSnapshot().live == 0);
}

static void
concurrent_and_cross_thread_free()
{
	DlSdkMemoryLedger ledger;
	std::array<void *, 8> pointers{};
	std::vector<std::thread> threads;
	for (std::size_t i = 0; i < pointers.size(); ++i)
		threads.emplace_back([&, i]() {
			pointers[i] = ledger.Allocate(8192, 64);
			assert(pointers[i] != nullptr);
			for (int j = 0; j < 10000; ++j)
			{
				void *p = ledger.Allocate(1 + j % 4096, 64);
				assert(p != nullptr);
				std::memset(p, 0x53, 1 + j % 4096);
				ledger.Free(p);
			}
		});
	for (auto &thread : threads)
		thread.join();
	auto snapshot = ledger.TakeSnapshot();
	assert(snapshot.live > pointers.size() * 8192);
	assert(snapshot.peak >= snapshot.live);
	/* Owner frees memory allocated by workers. */
	for (void *p : pointers)
		ledger.Free(p);
	assert(ledger.TakeSnapshot().live == 0);
}

static void
reservation_failure_and_peak()
{
	DlSdkMemoryAccount account;
	std::size_t charged = 0;
	std::size_t maximum_charge = 0;
	std::size_t allowance = 1024;
	const auto owner = std::this_thread::get_id();
	auto reserve = [&](std::size_t bytes) {
		assert(std::this_thread::get_id() == owner);
		if (bytes > allowance - charged)
			return false;
		charged += bytes;
		maximum_charge = std::max(maximum_charge, charged);
		return true;
	};
	auto release = [&](std::size_t bytes) {
		assert(std::this_thread::get_id() == owner);
		assert(bytes <= charged);
		charged -= bytes;
	};
	assert(account.Reconcile({256, 512}, reserve, release));
	assert(charged == 256 && maximum_charge == 512);
	/* Failure rolls back no other consumer's reservation. */
	assert(!account.Reconcile({512, 2048}, reserve, release));
	assert(charged == 256 && account.reserved() == 256);
	allowance = 4096;
	assert(account.Reconcile({512, 2048}, reserve, release));
	assert(charged == 512 && maximum_charge == 2048);
	assert(account.Reconcile({0, 512}, reserve, release));
	assert(charged == 0 && account.reserved() == 0);
	assert(account.Reconcile({512, 512}, reserve, release));
	allowance = 1024;
	assert(!account.Reconcile({0, 8192}, reserve, release));
	assert(charged == 0 && account.reserved() == 0);
}

#ifdef DL_TEST_AWS
static void
aws_containers_use_manager()
{
	DlAwsMemoryManager manager;
	Aws::Utils::Memory::InitializeAWSMemorySystem(manager);
	assert(Aws::Utils::Memory::GetMemorySystem() == &manager);
	{
		Aws::String text(1024 * 1024, 'x');
		Aws::Vector<int> values(65536, 42);
		auto object = Aws::MakeShared<Aws::String>("sdk_memory_test", 8192, 'y');
		assert(text.front() == 'x' && values.back() == 42);
		assert(object->size() == 8192);
		assert(manager.ledger.TakeSnapshot().live > 1024 * 1024 + 65536 * sizeof(int));
		void *crt = aws_mem_acquire(Aws::get_aws_allocator(), 1024 * 1024);
		assert(crt != nullptr);
		assert(manager.ledger.TakeSnapshot().live > 2 * 1024 * 1024);
		aws_mem_release(Aws::get_aws_allocator(), crt);
	}
	assert(manager.ledger.TakeSnapshot().live == 0);
	Aws::Utils::Memory::ShutdownAWSMemorySystem();
}
#endif

int
main()
{
	alignment_and_lifetime();
	invalid_requests();
	concurrent_and_cross_thread_free();
	reservation_failure_and_peak();
#ifdef DL_TEST_AWS
	aws_containers_use_manager();
#endif
	std::puts("sdk_memory_test: passed");
}
