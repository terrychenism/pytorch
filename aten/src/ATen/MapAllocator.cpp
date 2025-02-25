#include <ATen/MapAllocator.h>

#include <atomic>
#include <string>
#if ATOMIC_INT_LOCK_FREE == 2
#define AT_ATOMIC_IPC_REFCOUNT 1
#endif

#include <c10/core/CPUAllocator.h>
#include <c10/util/C++17.h>
#include <c10/util/Unicode.h>

/* stuff for mapped files */
#ifdef _WIN32
#include <c10/util/win32-headers.h>
#endif

#if defined(HAVE_MMAP)
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#if !defined(_MSC_VER) || defined(HAVE_MMAP)
#include <sys/types.h>
#include <unistd.h>
#elif defined(_MSC_VER)
#include <c10/util/win32-headers.h>
#endif

namespace at {

static constexpr int64_t map_alloc_alignment = 64;

TORCH_API std::string NewProcessWideShmHandle()
{
  static std::atomic<uint64_t> counter;
  std::string handle = "/torch_";
#ifdef _MSC_VER
  handle += c10::guts::to_string(GetCurrentProcessId());
#else
  handle += c10::guts::to_string(getpid());
#endif
  handle += "_";
  handle += c10::guts::to_string(counter.fetch_add(1, std::memory_order_relaxed));
  return handle;
}

#if defined(_WIN32) || defined(HAVE_MMAP)

namespace {
struct MapInfo {
  std::atomic<int> refcount;
};

const std::string unknown_filename = "filename not specified";
#ifdef _WIN32
const std::string unknown_eventname = "eventname not specified";
#endif
}  // namespace (anonymous)

MapAllocator::MapAllocator(WithFd, std::string filename, int fd, int flags, size_t size)
  : filename_(filename.empty() ? unknown_filename : std::move(filename))
  , flags_(0) // to be filled later
  , size_(0) // to be filled later
#ifdef _WIN32
  , handle_(INVALID_HANDLE_VALUE) // to be filled later
  , event_(INVALID_HANDLE_VALUE) // to be filled later
  , eventname_(filename.empty() ? unknown_eventname : (filename + "_event"))
#else
  , fd_(fd)
#endif
  , base_ptr_(nullptr)
{

  if (!(flags & ALLOCATOR_MAPPED_SHARED) && !(flags & ALLOCATOR_MAPPED_SHAREDMEM)) {
    flags &= ~ALLOCATOR_MAPPED_NOCREATE;
  }
  if ((flags ^ ALLOCATOR_MAPPED_EXCLUSIVE) == 0) {
    TORCH_CHECK(false, "ALLOCATOR_MAPPED_EXCLUSIVE flag requires opening the file in shared mode");
  }
#ifdef _WIN32
  if (fd != -1) {
    TORCH_CHECK(false, "MapAllocator_newWithFd is unsupported on Windows");
  }
#endif
  flags_ = flags;

  // OK, now do the allocation

  if (size == 0) {
    return;
  }

#ifdef _WIN32
  if (flags_ & ALLOCATOR_MAPPED_SHAREDMEM) {
    // Shadowing
    const wchar_t *filename;
    const wchar_t *eventname;
    const std::wstring wFilename = c10::u8u16(filename_);
    const std::wstring wEventname = c10::u8u16(eventname_);
    LARGE_INTEGER hfilesz;

    if (filename_[0] == '/') {
      filename = wFilename.c_str() + 1;
      eventname = wEventname.c_str() + 1;
    } else {
      filename = wFilename.c_str();
      eventname = wEventname.c_str();
    }

    hfilesz.QuadPart = size;

    if (flags_ & ALLOCATOR_MAPPED_EXCLUSIVE) {
      event_ = CreateEventW(nullptr, FALSE, FALSE, eventname);
    } else if (flags_ & ALLOCATOR_MAPPED_NOCREATE) {
      event_ = OpenEventW(EVENT_ALL_ACCESS, FALSE, eventname);
    } else {
      TORCH_CHECK(false, "Expected either ALLOCATOR_MAPPED_EXCLUSIVE or ALLOCATOR_MAPPED_NOCREATE");
    }

    if (event_ == nullptr) {
      TORCH_CHECK(false, "Couldn't open shared event: <", eventname, ">, error code: <", GetLastError(), ">");
    }

    if (flags_ & ALLOCATOR_MAPPED_EXCLUSIVE) {
      handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hfilesz.HighPart, hfilesz.LowPart, filename);
    } else if (flags_ & ALLOCATOR_MAPPED_NOCREATE) {
      handle_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, filename);
    } else {
      TORCH_CHECK(false, "Expected either ALLOCATOR_MAPPED_EXCLUSIVE or ALLOCATOR_MAPPED_NOCREATE");
    }

    if (handle_ == nullptr) {
      TORCH_CHECK(false, "Couldn't open shared file mapping: <", filename, ">, error code: <", GetLastError(), ">");
    }

    size_ = size;
    base_ptr_ = MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!base_ptr_) {
      TORCH_CHECK(false, "Couldn't map view of shared file <", filename, ">, error code: <", GetLastError(), ">");
    }
  } else {

    HANDLE hfile;
    HANDLE hmfile;
    LARGE_INTEGER hfilesz;

    if (flags_ & ALLOCATOR_MAPPED_EXCLUSIVE) {
      TORCH_CHECK(false, "exclusive file mapping is not supported on Windows");
    }
    if (flags_ & ALLOCATOR_MAPPED_NOCREATE) {
      TORCH_CHECK(false, "file mapping without creation is not supported on Windows");
    }
    if (flags_ & ALLOCATOR_MAPPED_KEEPFD) {
      TORCH_CHECK(false, "ALLOCATOR_MAPPED_KEEPFD not supported on Windows");
    }
    if (flags_ & ALLOCATOR_MAPPED_FROMFD) {
      TORCH_CHECK(false, "ALLOCATOR_MAPPED_FROMFD not supported on Windows");
    }

    // Shadowing
    const wchar_t *filename;
    const std::wstring wFilename = c10::u8u16(filename_);

    filename = wFilename.c_str();

    /* open file */
    /* FILE_FLAG_RANDOM_ACCESS ? */
    if (flags_) {
      hfile = CreateFileW(filename, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
      if (hfile == INVALID_HANDLE_VALUE) {
        TORCH_CHECK(false, "could not open file <", filename_, "> in read-write mode; error code: <", GetLastError(), ">");
      }
    } else {
      hfile = CreateFileW(filename, GENERIC_READ, FILE_SHARE_WRITE|FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
      if (hfile == INVALID_HANDLE_VALUE) {
        TORCH_CHECK(false, "could not open file <", filename_, "> in read-only mode; error code: <", GetLastError(), ">");
      }
    }

    if (GetFileSizeEx(hfile, &hfilesz) == 0) {
      TORCH_CHECK(false, "could not get file size: <", filename_, ">; error code: <", GetLastError(), ">");
    }

    if (size > 0) {
      if (size > hfilesz.QuadPart) {
        if (flags_) {
          hfilesz.QuadPart = size;
          if (SetFilePointerEx(hfile, hfilesz, NULL, FILE_BEGIN) == 0) {
            CloseHandle(hfile);
            TORCH_CHECK(false, "unable to stretch file <", filename_, "> to the right size; error code: <", GetLastError(), ">", filename_);
          }
          if (SetEndOfFile(hfile) == 0) {
            CloseHandle(hfile);
            TORCH_CHECK(false, "unable to write to file <", filename_, ">; error code: <", GetLastError(), ">");
          }
        } else {
          CloseHandle(hfile);
          TORCH_CHECK(false, "file <", filename_, "> size is smaller than the required mapping size <", size, ">; error code: <", GetLastError(), ">");
        }
      }
    } else {
      size = hfilesz.QuadPart;
    }

    size_ = size; /* if we are here, it must be the right size */

    hfilesz.QuadPart = size_;

    /* get map handle */
    if (flags_) {
      if ( (hmfile = CreateFileMappingW(hfile, NULL, PAGE_READWRITE, hfilesz.HighPart, hfilesz.LowPart, NULL)) == NULL ) {
        TORCH_CHECK(false, "could not create a map on file <", filename_, ">; error code: <", GetLastError(), ">");
      }
    } else {
      if ( (hmfile = CreateFileMappingW(hfile, NULL, PAGE_WRITECOPY, hfilesz.HighPart, hfilesz.LowPart, NULL)) == NULL ) {
        TORCH_CHECK(false, "could not create a map on file <", filename_, ">; error code: <", GetLastError(), ">");
      }
    }

    /* map the stuff */
    if(flags_) {
      base_ptr_ = MapViewOfFile(hmfile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    } else {
      base_ptr_ = MapViewOfFile(hmfile, FILE_MAP_COPY, 0, 0, 0);
    }

    CloseHandle(hfile);
    CloseHandle(hmfile);
  }
#else /* _WIN32 */
  {
    /* open file */
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    int fd;
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    int flags; // shadow
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct stat file_stat;

    if (flags_ & (ALLOCATOR_MAPPED_SHARED | ALLOCATOR_MAPPED_SHAREDMEM)) {
      flags = O_RDWR | O_CREAT;
    } else {
      flags = O_RDONLY;
    }

    if (flags_ & ALLOCATOR_MAPPED_EXCLUSIVE) {
      flags |= O_EXCL;
    }
    if (flags_ & ALLOCATOR_MAPPED_NOCREATE) {
      flags &= ~O_CREAT;
    }

    if (!(flags_ & ALLOCATOR_MAPPED_FROMFD)) {
      if (flags_ & ALLOCATOR_MAPPED_SHARED) {
        if ((fd = open(filename_.c_str(), flags, (mode_t)0600)) == -1) {
          TORCH_CHECK(false, "unable to open file <", filename_, "> in read-write mode");
        }
      } else if (flags_ & ALLOCATOR_MAPPED_SHAREDMEM) {
#ifdef HAVE_SHM_OPEN
        if((fd = shm_open(filename_.c_str(), flags, (mode_t)0600)) == -1) {
          TORCH_CHECK(false, "unable to open shared memory object <", filename_, "> in read-write mode");
        }
#else
        TORCH_CHECK(false, "unable to open file <", filename_, "> in sharedmem mode, shm_open unavailable on this platform");
#endif
      } else {
        if ((fd = open(filename_.c_str(), O_RDONLY)) == -1) {
          TORCH_CHECK(false, "unable to open file <", filename_, "> in read-only mode");
        }
      }
    } else {
      fd = fd_;
    }

    if (fstat(fd, &file_stat) == -1) {
      if (!(flags_ & ALLOCATOR_MAPPED_FROMFD)) {
        ::close(fd);
      }
      TORCH_CHECK(false, "unable to stat the file <", filename_, ">");
    }

    if (size > 0) {
      if (static_cast<int64_t>(size) > file_stat.st_size) {
        if (flags_) {
          if (ftruncate(fd, size) == -1) {
            TORCH_CHECK(false, "unable to resize file <", filename_, "> to the right size");
          }
          if (fstat(fd, &file_stat) == -1 || file_stat.st_size < static_cast<int64_t>(size)) {
            ::close(fd);
            TORCH_CHECK(false, "unable to stretch file <", filename_, "> to the right size");
          }
/* on macOS write returns with errno 45 (Opperation not supported) when used
 * with a file descriptor obtained via shm_open
 */
#ifndef __APPLE__
          if ((write(fd, "", 1)) != 1) /* note that the string "" contains the '\0' byte ... */ {
            ::close(fd);
            TORCH_CHECK(false, "unable to write to file <", filename_, ">");
          }
#endif
        } else {
          ::close(fd);
          TORCH_CHECK(false, "file <", filename_, "> size is smaller than the required mapping size <", size, ">");
        }
      }
    } else {
      size = file_stat.st_size;
    }

    size_ = size; /* if we are here, it must be the right size */

    /* map it */
    if (flags_ & (ALLOCATOR_MAPPED_SHARED | ALLOCATOR_MAPPED_SHAREDMEM)) {
      base_ptr_ = mmap(nullptr, size_, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    } else {
      base_ptr_ = mmap(nullptr, size_, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
    }

    if (base_ptr_ == MAP_FAILED) {
      base_ptr_ = nullptr; /* let's be sure it is NULL */
      TORCH_CHECK(false, "unable to mmap ", size_, " bytes from file <", filename_, ">: ", strerror(errno), " (", errno, ")");
    }

    if (flags_ & ALLOCATOR_MAPPED_KEEPFD) {
      fd_ = fd;
    } else {
      if (::close(fd) == -1) {
        TORCH_CHECK(false, "Error closing file <", filename_, ">");
      }
      fd_ = -1;
    }

    if (flags_ & ALLOCATOR_MAPPED_UNLINK) {
      if (flags_ & ALLOCATOR_MAPPED_SHAREDMEM) {
#ifdef HAVE_SHM_UNLINK
        if (shm_unlink(filename_.c_str()) == -1) {
          TORCH_CHECK(false, "could not unlink the shared memory file ", filename_);
        }
#else
        TORCH_CHECK(false, "could not unlink the shared memory file ", filename_, ", shm_unlink not available on platform");
#endif
      } else {
        if (unlink(filename_.c_str()) == -1)
          TORCH_CHECK(false, "could not unlink file %s", filename_);
      }
    }

    if (base_ptr_ == MAP_FAILED) {
      TORCH_CHECK(false, "$ Torch: unable to mmap memory: you tried to mmap ", size_/1073741824, " GB.");
    }
  }
#endif
  c10::reportMemoryUsageToProfiler(base_ptr_, size_, 0, size_, c10::Device(c10::DeviceType::CPU));
}

MapAllocator::MapAllocator(std::string filename, int flags, size_t size)
  : MapAllocator(WITH_FD, std::move(filename), -1, flags, size)
{}

#ifdef _WIN32
struct ReleaseContext {
  HANDLE event;
  HANDLE handle;
  HANDLE wait;
};
static void CALLBACK WaitForReleaseHandle(PVOID lpParam, BOOLEAN TimerOrWaitFired)
{
  if (lpParam) {
    ReleaseContext *ctx = (ReleaseContext *)lpParam;

    SetEvent(ctx->event);
    CloseHandle(ctx->event);
    CloseHandle(ctx->handle);

    UnregisterWait(ctx->wait);

    delete ctx;
  }
}
#endif

void MapAllocator::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  if (base_ptr_ == nullptr) {
    return;
  }
#ifdef _WIN32
  if ((flags_ & ALLOCATOR_MAPPED_KEEPFD) || (flags_ & ALLOCATOR_MAPPED_SHAREDMEM))
    CloseHandle(handle_);
  if(UnmapViewOfFile(base_ptr_) == 0)
    TORCH_CHECK(false, "could not unmap the shared memory file");
#else /* _WIN32 */
  if (flags_ & ALLOCATOR_MAPPED_KEEPFD) {
    if (::close(fd_) == -1) {
      TORCH_CHECK(false, "could not close file descriptor ", fd_);
    }
  }

  if (munmap(base_ptr_, size_)) {
    TORCH_CHECK(false, "could not unmap the shared memory file");
  }

  if (!(flags_ & (ALLOCATOR_MAPPED_FROMFD | ALLOCATOR_MAPPED_UNLINK))) {
    if (flags_ & ALLOCATOR_MAPPED_SHAREDMEM) {
#ifdef HAVE_SHM_UNLINK
      if (shm_unlink(filename_.c_str()) == -1) {
        TORCH_CHECK(false, "could not unlink the shared memory file ", filename_);
      }
#else
      TORCH_CHECK(false, "could not unlink the shared memory file ", filename_, ", shm_unlink not available on platform");
#endif
    }
  }
#endif /* _WIN32 */
}

#else /* defined(_WIN32) || defined(HAVE_MMAP) */

MapAllocator::MapAllocator(std::string filename, int flags, size_t size) {
  TORCH_CHECK(false, "file mapping not supported on your system");
}

MapAllocator::MapAllocator(WithFd, std::string filename, int fd, int flags, size_t size) {
  TORCH_CHECK(false, "file mapping not supported on your system");
}

void MapAllocator::close() { }

#endif

#if (defined(_WIN32) || defined(HAVE_MMAP)) && defined(AT_ATOMIC_IPC_REFCOUNT)

RefcountedMapAllocatorArgCheck::RefcountedMapAllocatorArgCheck(int flags) {
  if (flags & ALLOCATOR_MAPPED_FROMFD) {
    TORCH_CHECK(false, "RefcountedMapAllocator doesn't support ALLOCATOR_MAPPED_FROMFD flag");
  }
  if (flags & ALLOCATOR_MAPPED_KEEPFD) {
    TORCH_CHECK(false, "RefcountedMapAllocator doesn't support ALLOCATOR_MAPPED_KEEPFD flag");
  }
  if (flags & ALLOCATOR_MAPPED_UNLINK) {
    TORCH_CHECK(false, "RefcountedMapAllocator doesn't support ALLOCATOR_MAPPED_UNLINK flag");
  }
  if (!(flags & ALLOCATOR_MAPPED_SHAREDMEM)) {
    TORCH_CHECK(false, "RefcountedMapAllocator requires ALLOCATOR_MAPPED_SHAREDMEM flag");
  }
}

RefcountedMapAllocator::RefcountedMapAllocator(const char *filename, int flags, size_t size)
  : RefcountedMapAllocatorArgCheck(flags)
  , MapAllocator(filename, flags, size + map_alloc_alignment) {

    initializeAlloc();
}
RefcountedMapAllocator::RefcountedMapAllocator(WithFd, const char *filename, int fd, int flags, size_t size)
  : RefcountedMapAllocatorArgCheck(flags)
  , MapAllocator(WITH_FD, filename, flags, fd, size + map_alloc_alignment) {

    initializeAlloc();
}

void RefcountedMapAllocator::initializeAlloc() {
  MapInfo *map_info = (MapInfo*)base_ptr_;

#ifdef _WIN32
  ReleaseContext* r_ctx = new ReleaseContext;
  r_ctx->handle = handle_;
  r_ctx->event = event_;
  r_ctx->wait = NULL;
  BOOL can_wait = RegisterWaitForSingleObject(&r_ctx->wait, event_, WaitForReleaseHandle, (PVOID)r_ctx, INFINITE, WT_EXECUTEONLYONCE);
  TORCH_CHECK(can_wait, "Couldn't register wait on event, error code: <", GetLastError(), ">");
#endif

  if (flags_ & ALLOCATOR_MAPPED_EXCLUSIVE) {
    new (&map_info->refcount) std::atomic<int>(1);
  } else {
    map_info->refcount++;
  }
}

void RefcountedMapAllocator::close() {
  if (closed_) {
    return;
  }
  closed_ = true;

  void* data = base_ptr_;

#ifdef _WIN32
  MapInfo *info = (MapInfo*)data;
  if (--info->refcount == 0) {
    SetEvent(event_);
  }
  if(UnmapViewOfFile(data) == 0) {
    TORCH_CHECK(false, "could not unmap the shared memory file");
  }
#else /* _WIN32 */

  MapInfo *info = (MapInfo*)(data);
  if (--info->refcount == 0) {
#ifdef HAVE_SHM_UNLINK
    if (shm_unlink(filename_.c_str()) == -1) {
      TORCH_CHECK(false, "could not unlink the shared memory file ", filename_);
    }
#else
    TORCH_CHECK(false, "could not unlink the shared memory file ", filename_, ", shm_unlink not available on platform");
#endif /* HAVE_SHM_UNLINK */
  }
  if (munmap(info, size_)) {
    TORCH_CHECK(false, "could not unmap the shared memory file ", filename_);
  }
#endif /* _WIN32 */
}

void RefcountedMapAllocator::incref()
{
  MapInfo *map_info = static_cast<MapInfo*>(base_ptr_);
  ++map_info->refcount;
}

int RefcountedMapAllocator::decref()
{
  MapInfo *map_info = static_cast<MapInfo*>(base_ptr_);
  return --map_info->refcount == 0;
}

#else


RefcountedMapAllocatorArgCheck::RefcountedMapAllocatorArgCheck(int flags) {}

RefcountedMapAllocator::RefcountedMapAllocator(const char *filename, int flags, size_t size)
  : RefcountedMapAllocatorArgCheck(flags),
    MapAllocator(filename, flags, size + map_alloc_alignment)
{
  TORCH_CHECK(false, "refcounted file mapping not supported on your system");
}

RefcountedMapAllocator::RefcountedMapAllocator(WithFd, const char *filename, int fd, int flags, size_t size)
  : RefcountedMapAllocatorArgCheck(flags),
    MapAllocator(WITH_FD, filename, flags, fd, size + map_alloc_alignment)
{
  TORCH_CHECK(false, "refcounted file mapping not supported on your system");
}

void RefcountedMapAllocator::initializeAlloc() {}

void RefcountedMapAllocator::close() {}

#endif

static void deleteMapAllocator(void* ptr) {
  delete static_cast<MapAllocator*>(ptr);
}

static void deleteRefcountedMapAllocator(void* ptr) {
  delete static_cast<RefcountedMapAllocator*>(ptr);
}

MapAllocator* MapAllocator::fromDataPtr(const at::DataPtr& dptr) {
  return dptr.cast_context<MapAllocator>(&deleteMapAllocator);
}

RefcountedMapAllocator* RefcountedMapAllocator::fromDataPtr(const at::DataPtr& dptr) {
  return dptr.cast_context<RefcountedMapAllocator>(&deleteRefcountedMapAllocator);
}

at::DataPtr MapAllocator::makeDataPtr(std::string filename, int flags, size_t size, size_t* actual_size_out) {
  auto* context = new MapAllocator(std::move(filename), flags, size);
  if (actual_size_out) *actual_size_out = context->size();
  return {context->data(), context, &deleteMapAllocator, at::DeviceType::CPU};
}

at::DataPtr MapAllocator::makeDataPtr(WithFd, const char *filename, int fd, int flags, size_t size, size_t* actual_size_out) {
  auto* context = new MapAllocator(WITH_FD, filename, fd, flags, size);
  if (actual_size_out) *actual_size_out = context->size();
  return {context->data(), context, &deleteMapAllocator, at::DeviceType::CPU};
}

at::DataPtr RefcountedMapAllocator::makeDataPtr(const char *filename, int flags, size_t size, size_t* actual_size_out) {
  auto* context = new RefcountedMapAllocator(filename, flags, size);
  if (actual_size_out) *actual_size_out = context->size() - map_alloc_alignment;
  return {context->data(), context, &deleteRefcountedMapAllocator, at::DeviceType::CPU};
}

at::DataPtr RefcountedMapAllocator::makeDataPtr(WithFd, const char *filename, int fd, int flags, size_t size, size_t* actual_size_out) {
  auto* context = new RefcountedMapAllocator(WITH_FD, filename, fd, flags, size);
  if (actual_size_out) *actual_size_out = context->size() - map_alloc_alignment;
  return {context->data(), context, &deleteRefcountedMapAllocator, at::DeviceType::CPU};
}

void* RefcountedMapAllocator::data() const {
  return static_cast<void*>(static_cast<char*>(base_ptr_) + map_alloc_alignment);
}

MapAllocator::~MapAllocator() {
  // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.VirtualCall)
  close();
  c10::reportMemoryUsageToProfiler(base_ptr_, -size_, 0, 0, c10::Device(c10::DeviceType::CPU));
}

}  // namespace at
