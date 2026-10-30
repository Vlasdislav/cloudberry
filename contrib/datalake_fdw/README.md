<!--
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing,
  software distributed under the License is distributed on an
  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
  KIND, either express or implied.  See the License for the
  specific language governing permissions and limitations
  under the License.
-->

# datalake_fdw

Apache Iceberg lake tables as a Cloudberry extension.  This document covers
the storage layer: where a lake table's files live, how the extension is told
to reach them, and how to add a kind of storage it does not know about.

## Volumes

A volume is a foreign server that says where data lives and how to get in.
Every path the extension reads or writes belongs to one.

```sql
CREATE SERVER warehouse
    FOREIGN DATA WRAPPER iceberg_volume_fdw
    OPTIONS (base_path 's3://analytics/warehouse',
             endpoint 'https://s3.eu-central-1.amazonaws.com',
             region 'eu-central-1',
             path_style_access 'false');

CREATE USER MAPPING FOR analyst SERVER warehouse
    OPTIONS (access_key_id 'AKIA...', secret_access_key '...');
```

Server options:

| Option | Meaning |
|---|---|
| `base_path` | Required.  A URI: `s3://bucket/prefix` or `file:///mnt/warehouse`.  Its scheme selects the storage backend, so there is no separate option naming the protocol. |
| `endpoint` | The service to talk to, when it is not AWS.  `http://` selects plain HTTP; anything else is HTTPS. |
| `region` | Defaults to `us-east-1`. |
| `path_style_access` | `true` addresses a bucket as a path, `false` as a host name.  Unset means path style when an `endpoint` is given and host style when it is not, which is what AWS and everything else respectively want. |

User mapping options, all optional: `access_key_id`, `secret_access_key`,
`session_token`, `username`.

### Where credentials come from

In this order, and the first that applies wins:

1. the user mapping for the querying user;
2. the `PUBLIC` user mapping for that server;
3. whatever the host already grants -- environment variables, an instance
   profile, a shared credentials file, a web identity token.

A volume with no user mapping is therefore not a misconfiguration: it is how
an instance profile is meant to be used.  Where none of the three yields a
credential, the request is made unsigned and the service refuses it.

On a host that is not on EC2, the default chain will try the instance metadata
service and wait for it to time out.  Set `AWS_EC2_METADATA_DISABLED=true` in
the server's environment to skip that.

### `file://` volumes

A `file://` volume is a directory, and every segment reads and writes it
directly.  It is only correct if that directory is the *same* directory on
every host -- a network filesystem, or a cluster filesystem.  A path that
happens to exist on each host separately will produce a table whose files
exist in several places and nowhere in full.  Nothing checks this; it is
yours to arrange.

## Building with S3 support

S3 needs the AWS SDK for C++.  No distribution packages it, so unless you are
using an image that already has it, build it once:

```sh
git clone --depth 1 --branch 1.11.844 --recurse-submodules --shallow-submodules \
    https://github.com/aws/aws-sdk-cpp.git
cmake -S aws-sdk-cpp -B aws-sdk-cpp/build -GNinja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/datalake \
    -DBUILD_ONLY="s3;sts" -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DENABLE_TESTING=OFF -DUSE_OPENSSL=ON -DCUSTOM_MEMORY_MANAGEMENT=ON
ninja -C aws-sdk-cpp/build install
```

It needs libcurl, OpenSSL and zlib headers, and takes well under a minute to
compile; the clone is the slow part.  1.11.844 is the version this extension
is tested against, and 1.11 is the minimum.

The extension finds it under `/usr/local` or `/opt/datalake`, or wherever
`AWS_SDK_PREFIX` says:

```sh
make USE_PGXS=1 AWS_SDK_PREFIX=/opt/datalake install
```

Without the SDK the extension still builds -- the build says so -- and opening
an `s3://` location then fails with a message telling you to rebuild.  Naming
a prefix that has no SDK in it is an error rather than a silent fallback.

Arrow itself needs no S3 support: this extension does not use Arrow's S3
filesystem, which is just as well, because no RPM of Arrow is built with it.

The SDK must be built with `CUSTOM_MEMORY_MANAGEMENT=ON`. Rebuild an existing
SDK installation that used the default `OFF`: this changes the allocator types
of `Aws::String` and other SDK containers, so the SDK and every consumer must
agree. Defining `USE_AWS_MEMORY_MANAGEMENT` only when building the extension is
not sufficient. The extension refuses SDK headers without this setting, and
also checks that its manager was installed at runtime.

## AWS SDK memory accounting

The S3 backend installs an AWS `MemorySystemInterface`. Allocation callbacks
record live bytes and the peak since the preceding checkpoint, including the
manager's per-allocation header and alignment padding. Callbacks do not call
PostgreSQL, so allocation and freeing on SDK worker threads are supported.
The manager lives until process exit; it is not allocated in a query's memory
context.

On the PostgreSQL thread, SDK initialization, mounting, and request boundaries
reconcile those counters with `VmemTracker_ReserveVmem` and
`VmemTracker_ReleaseVmem`, the same tracker the Arrow pool uses. A checkpoint
first tries to reserve the observed peak, then releases the difference from
the currently live allocation size. Failed reservations are never released as
though they had succeeded. Pending PostgreSQL interrupts are held while
reserving, as in the Arrow pool.

This is **deferred accounting, not an allocation-time hard limit**. The SDK
is compiled without C++ exception handling, and its allocation consumers do
not consistently handle `NULL`. A memory quota must not make a callback
`longjmp`, throw through SDK frames, or return `NULL` to cancel a query.
Consequently, SDK allocations use the system allocator. Reads check pressure
after returning from the SDK. Mutating calls defer reporting it until before
the next SDK request: the completed request keeps its result, so a created
upload ID or a completed write is not lost to an accounting error. A final
mutating request can complete before pressure is reported; an idle session
need not observe the pending error until its next S3 operation.
Abort requests bypass the pressure check so multipart cleanup remains possible.
Temporary memory can exceed the quota within a single call. System allocation
failure retains the SDK's existing OOM limitations.

This uses the existing process/session vmem policy, including its behavior
when resource groups are active. It does not introduce an independent hard
resource-group limit or transfer persistent SDK allocations between groups.
SDK 1.11.844 also routes its CRT allocator and its libcurl allocation callbacks
through the manager when it controls their initialization. This does not
guarantee coverage of independently initialized dependencies, OpenSSL internals,
or ordinary `std::` containers in the extension. Arrow buffers, including the
multipart data buffer, continue to use the tracked Arrow pool and are not
counted twice.

Run the standalone accounting tests without a cluster:

```sh
make -C test/unit check
```

With a configured extension build and a custom-memory SDK, also verify that
real AWS containers use the manager:

```sh
make USE_PGXS=1 AWS_SDK_PREFIX=/opt/datalake check-sdk-memory
```

## Writing a storage backend

A backend is a shared library that answers one question: given a location and
its options, which `arrow::fs::FileSystem` reads and writes it.  Everything
else -- opening files, listing, error classification, memory accounting -- is
the extension's side of the boundary.

`make install` puts five headers under
`$(pg_config --includedir-server)/extension/datalake_fdw/`, and they are all a
backend includes:

```cpp
#include <arrow/filesystem/localfs.h>

#include "storage_backend.h"
#include "storage_backend_register.h"

extern "C" { PG_MODULE_MAGIC; void _PG_init(void); }

static arrow::Result<DatalakeMountedFs>
mount_mine(const DatalakeLocation *location, const DlKeyValue *kv, int nkv,
           const DatalakeStorageHost *host)
{
    DatalakeMountedFs mounted;

    mounted.fs = std::make_shared<arrow::fs::LocalFileSystem>(
        arrow::io::IOContext(dl_storage_host_pool(host)));
    mounted.root = location->path_prefix;
    return mounted;
}

static const DatalakeStorageBackend mine = {
    DL_STORAGE_ABI_VERSION, sizeof(DatalakeStorageBackend), "mine",
    ARROW_VERSION_STRING, DL_STORAGE_ABI_FINGERPRINT, mount_mine, NULL, NULL
};

void
_PG_init(void)
{
    DlErrCode rc = datalake_storage_register(&mine);

    if (rc != DL_OK)
        elog(ERROR, "could not register the \"mine\" storage backend");
}
```

Put the library in `shared_preload_libraries`.  The order does not matter:
registering pulls in `datalake_fdw` if it is not loaded yet.  Registration
only happens during preload, so that every backend process agrees on which
schemes exist.

`dltest/` in this directory is exactly that, built and installed as
`datalake_fdw_dltest`: a backend in a library of its own, compiled against the
installed headers and registering through `datalake_storage_register`.  The
regression suite preloads it, which is how the boundary described here is
tested rather than asserted.  It is not part of `datalake_fdw`, and a server
that does not name it has no `dltest` scheme.

What the extension calls on the filesystem it gets back, and nothing else:

* `GetFileInfo(path)` and `GetFileInfo(FileSelector)`
* `OpenInputFile`, then `GetSize`, `ReadAt` and `Read`
* `OpenOutputStream`, then `Write`, `Close` and `Abort`
* `DeleteFile`

Anything else may return `NotImplemented`.

The sketch above is the shape of a backend, not a correct one: Arrow's
`LocalFileSystem` truncates what it opens and its `Abort()` leaves the file
behind, so a backend built from it as written breaks the first two obligations
below.  `dltest/` is the worked example that does not.

Three obligations are worth stating, because a backend that gets them wrong
is wrong in ways tests elsewhere will not catch:

* **`Abort()` removes what this stream created, and only that.**  Nothing else
  deletes on a stream's behalf, because after a failed write the name may
  already belong to another writer.  If the stream never created anything --
  an upload that was never started -- abort does nothing.
* **Allocate through `host->pool`.**  Arrow's default pool is invisible to
  Cloudberry's memory accounting, and a query that allocates outside it is a
  query whose memory limit does not apply.
* **Classify failures.**  Return `arrow::Status::AlreadyExists` for a name in
  use, and for something missing an `IOError` whose text contains
  `does not exist` or `No such file or directory` -- those two phrases and
  nothing else, because the text also contains the caller's path and a status
  number or a service's error code matched inside it would turn an error
  *about* a path into an error about the path not existing.  Anything else is
  reported as an I/O error.

`abi_fingerprint` is checked at registration: the compiler's major version,
libstdc++'s dual-ABI setting and the Arrow version have to match the ones the
extension was built with, because `std::shared_ptr` and `arrow::Result` cross
the boundary by value.  Build a backend with the same toolchain and Arrow
package as the extension.

## Known limits

* The fingerprint catches the mismatches that occur in practice, not every
  possible one.  A backend built against a different C++ runtime can still
  register and then misbehave.
* Registering from inside a backend process instead of during preload affects
  only that process, and is not supported.
* Between checking that a name is free and creating it, another writer can
  take it.  Iceberg's file names are unique by construction, so this does not
  arise there.
* An upload abandoned by a crashed backend leaves its parts behind.  A bucket
  lifecycle rule that expires incomplete multipart uploads is the usual answer.
* SDK memory accounting is deferred to call boundaries. It neither prevents
  transient quota overshoot nor accounts for every allocation in the SDK's
  dependencies; see AWS SDK memory accounting above. No claim is made that
  all S3-related resident memory is covered by `gp_vmem_protect_limit`.
