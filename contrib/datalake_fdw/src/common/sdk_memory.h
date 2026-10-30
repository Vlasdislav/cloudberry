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

#ifndef DL_SDK_MEMORY_H
#define DL_SDK_MEMORY_H

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>

/*
 * The SDK can allocate on worker threads.  This ledger deliberately has no
 * PostgreSQL dependency: only its owner-thread consumer may touch vmem.
 * Metadata and alignment padding are counted as well as the requested bytes.
 */
class DlSdkMemoryLedger
{
public:

	struct Snapshot
	{
		std::size_t live;
		std::size_t peak;
	};

	void *Allocate(std::size_t bytes, std::size_t alignment)
	{
		alignment = std::max(alignment, alignof(std::max_align_t));
		if ((alignment & (alignment - 1)) != 0)
			return nullptr;
		const auto maximum = std::numeric_limits<std::size_t>::max();
		if (alignment - 1 > maximum - sizeof(Header))
			return nullptr;
		const auto overhead = sizeof(Header) + alignment - 1;
		bytes = std::max<std::size_t>(bytes, 1);
		if (bytes > maximum - overhead)
			return nullptr;
		const auto total = bytes + overhead;
		void *base = std::malloc(total);
		if (base == nullptr)
			return nullptr;

		auto start = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
		auto aligned = (start + alignment - 1) & ~(alignment - 1);
		Header header{base, total};
		/* memcpy avoids assumptions about the header's object lifetime. */
		std::memcpy(reinterpret_cast<void *>(aligned - sizeof(Header)),
					&header, sizeof(header));
		{
			std::lock_guard<std::mutex> lock(mutex_);
			live_ += total;
			peak_ = std::max(peak_, live_);
		}
		return reinterpret_cast<void *>(aligned);
	}

	void Free(void *pointer)
	{
		if (pointer == nullptr)
			return;
		Header header;
		std::memcpy(&header, static_cast<char *>(pointer) - sizeof(Header),
					sizeof(header));
		/* Never report fewer bytes than the allocator still owns. */
		std::free(header.base);
		std::lock_guard<std::mutex> lock(mutex_);
		assert(live_ >= header.total);
		live_ -= header.total;
	}

	Snapshot TakeSnapshot()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Snapshot snapshot{live_, peak_};
		peak_ = live_;
		return snapshot;
	}

private:
	struct Header
	{
		void *base;
		std::size_t total;
	};

	std::mutex mutex_;
	std::size_t live_ = 0;
	std::size_t peak_ = 0;
};

/*
 * Only the PostgreSQL owner thread uses this account. Reserve/release callbacks
 * make the arithmetic testable without starting a cluster. A failed reservation
 * must not later be released as though it had succeeded.
 */
class DlSdkMemoryAccount
{
public:
	template <typename Reserve, typename Release>
	bool Reconcile(DlSdkMemoryLedger::Snapshot snapshot,
				   Reserve reserve, Release release)
	{
		assert(snapshot.peak >= snapshot.live);
		if (snapshot.peak > reserved_)
		{
			if (!reserve(snapshot.peak - reserved_))
			{
				/* Cleanup may already have freed an older reservation. */
				if (reserved_ > snapshot.live)
				{
					release(reserved_ - snapshot.live);
					reserved_ = snapshot.live;
				}
				return false;
			}
			reserved_ = snapshot.peak;
		}
		if (reserved_ > snapshot.live)
		{
			release(reserved_ - snapshot.live);
			reserved_ = snapshot.live;
		}
		return true;
	}

	std::size_t reserved() const { return reserved_; }

private:
	std::size_t reserved_ = 0;
};

#endif /* DL_SDK_MEMORY_H */
