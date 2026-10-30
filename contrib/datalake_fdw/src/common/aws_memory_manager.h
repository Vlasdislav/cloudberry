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

#ifndef DL_AWS_MEMORY_MANAGER_H
#define DL_AWS_MEMORY_MANAGER_H

#include <aws/core/utils/memory/MemorySystemInterface.h>

#include "common/sdk_memory.h"

#ifndef USE_AWS_MEMORY_MANAGEMENT
#error "datalake_fdw requires an AWS SDK built with -DCUSTOM_MEMORY_MANAGEMENT=ON"
#endif

class DlAwsMemoryManager : public Aws::Utils::Memory::MemorySystemInterface
{
public:
	void Begin() override {}
	void End() override {}
	void *AllocateMemory(std::size_t bytes, std::size_t alignment,
						 const char *tag = nullptr) override
	{
		(void) tag;
		return ledger.Allocate(bytes, alignment);
	}
	void FreeMemory(void *pointer) override { ledger.Free(pointer); }

	DlSdkMemoryLedger ledger;
};

#endif /* DL_AWS_MEMORY_MANAGER_H */
