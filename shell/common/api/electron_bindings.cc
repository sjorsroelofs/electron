// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/common/api/electron_bindings.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/process/process_metrics_iocounters.h"
#include "base/system/sys_info.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/common/chrome_version.h"
#include "electron/electron_version.h"
#include "services/resource_coordinator/public/cpp/memory_instrumentation/global_memory_dump.h"
#include "services/resource_coordinator/public/cpp/memory_instrumentation/memory_instrumentation.h"
#include "shell/browser/browser.h"
#include "shell/common/application_info.h"
#include "shell/common/gin_converters/file_path_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/locker.h"
#include "shell/common/heap_snapshot.h"
#include "shell/common/node_includes.h"
#include "shell/common/promise_util.h"
#include "third_party/blink/renderer/platform/heap/process_heap.h"  // nogncheck

namespace electron {

namespace {

// Called when there is a fatal error in V8, we just crash the process here so
// we can get the stack trace.
void FatalErrorCallback(const char* location, const char* message) {
  LOG(ERROR) << "Fatal error in V8: " << location << " " << message;
  ElectronBindings::Crash();
}

}  // namespace

ElectronBindings::ElectronBindings(uv_loop_t* loop) {
  uv_async_init(loop, &call_next_tick_async_, OnCallNextTick);
  call_next_tick_async_.data = this;
  metrics_ = base::ProcessMetrics::CreateCurrentProcessMetrics();
}

ElectronBindings::~ElectronBindings() {
  uv_close(reinterpret_cast<uv_handle_t*>(&call_next_tick_async_), nullptr);
}

// static
void ElectronBindings::BindProcess(v8::Isolate* isolate,
                                   gin_helper::Dictionary* process,
                                   base::ProcessMetrics* metrics) {
  // These bindings are shared between sandboxed & unsandboxed renderers
  process->SetMethod("crash", &Crash);
  process->SetMethod("hang", &Hang);
  process->SetMethod("log", &Log);
  process->SetMethod("getCreationTime", &GetCreationTime);
  process->SetMethod("getHeapStatistics", &GetHeapStatistics);
  process->SetMethod("getBlinkMemoryInfo", &GetBlinkMemoryInfo);
  process->SetMethod("getProcessMemoryInfo", &GetProcessMemoryInfo);
  process->SetMethod("getSystemMemoryInfo", &GetSystemMemoryInfo);
  process->SetMethod("getSystemVersion",
                     &base::SysInfo::OperatingSystemVersion);
  process->SetMethod("getIOCounters", &GetIOCounters);
  process->SetMethod("getCPUUsage",
                     base::BindRepeating(&ElectronBindings::GetCPUUsage,
                                         base::Unretained(metrics)));

#if defined(MAS_BUILD)
  process->SetReadOnly("mas", true);
#endif

#if defined(OS_WIN)
  if (IsRunningInDesktopBridge())
    process->SetReadOnly("windowsStore", true);
#endif
}

void ElectronBindings::BindTo(v8::Isolate* isolate,
                              v8::Local<v8::Object> process) {
  isolate->SetFatalErrorHandler(FatalErrorCallback);

  gin_helper::Dictionary dict(isolate, process);
  BindProcess(isolate, &dict, metrics_.get());

  dict.SetMethod("takeHeapSnapshot", &TakeHeapSnapshot);
#if defined(OS_POSIX)
  dict.SetMethod("setFdLimit", &base::IncreaseFdLimitTo);
#endif
  dict.SetMethod("activateUvLoop",
                 base::BindRepeating(&ElectronBindings::ActivateUVLoop,
                                     base::Unretained(this)));

  gin_helper::Dictionary versions;
  if (dict.Get("versions", &versions)) {
    versions.SetReadOnly(ELECTRON_PROJECT_NAME, ELECTRON_VERSION_STRING);
    versions.SetReadOnly("chrome", CHROME_VERSION_STRING);
  }
}

void ElectronBindings::EnvironmentDestroyed(node::Environment* env) {
  auto it =
      std::find(pending_next_ticks_.begin(), pending_next_ticks_.end(), env);
  if (it != pending_next_ticks_.end())
    pending_next_ticks_.erase(it);
}

void ElectronBindings::ActivateUVLoop(v8::Isolate* isolate) {
  node::Environment* env = node::Environment::GetCurrent(isolate);
  if (std::find(pending_next_ticks_.begin(), pending_next_ticks_.end(), env) !=
      pending_next_ticks_.end())
    return;

  pending_next_ticks_.push_back(env);
  uv_async_send(&call_next_tick_async_);
}

// static
void ElectronBindings::OnCallNextTick(uv_async_t* handle) {
  ElectronBindings* self = static_cast<ElectronBindings*>(handle->data);
  for (std::list<node::Environment*>::const_iterator it =
           self->pending_next_ticks_.begin();
       it != self->pending_next_ticks_.end(); ++it) {
    node::Environment* env = *it;
    gin_helper::Locker locker(env->isolate());
    v8::Context::Scope context_scope(env->context());
    node::InternalCallbackScope scope(
        env, v8::Local<v8::Object>(), {0, 0},
        node::InternalCallbackScope::kAllowEmptyResource);
  }

  self->pending_next_ticks_.clear();
}

// static
void ElectronBindings::Log(const base::string16& message) {
  std::cout << message << std::flush;
}

// static
void ElectronBindings::Crash() {
  volatile int* zero = nullptr;
  *zero = 0;
}

// static
void ElectronBindings::Hang() {
  for (;;)
    base::PlatformThread::Sleep(base::TimeDelta::FromSeconds(1));
}

// static
v8::Local<v8::Value> ElectronBindings::GetHeapStatistics(v8::Isolate* isolate) {
  v8::HeapStatistics v8_heap_stats;
  isolate->GetHeapStatistics(&v8_heap_stats);

  gin_helper::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
  dict.SetHidden("simple", true);
  dict.Set("totalHeapSize",
           static_cast<double>(v8_heap_stats.total_heap_size() >> 10));
  dict.Set(
      "totalHeapSizeExecutable",
      static_cast<double>(v8_heap_stats.total_heap_size_executable() >> 10));
  dict.Set("totalPhysicalSize",
           static_cast<double>(v8_heap_stats.total_physical_size() >> 10));
  dict.Set("totalAvailableSize",
           static_cast<double>(v8_heap_stats.total_available_size() >> 10));
  dict.Set("usedHeapSize",
           static_cast<double>(v8_heap_stats.used_heap_size() >> 10));
  dict.Set("heapSizeLimit",
           static_cast<double>(v8_heap_stats.heap_size_limit() >> 10));
  dict.Set("mallocedMemory",
           static_cast<double>(v8_heap_stats.malloced_memory() >> 10));
  dict.Set("peakMallocedMemory",
           static_cast<double>(v8_heap_stats.peak_malloced_memory() >> 10));
  dict.Set("doesZapGarbage",
           static_cast<bool>(v8_heap_stats.does_zap_garbage()));

  return dict.GetHandle();
}

// static
v8::Local<v8::Value> ElectronBindings::GetCreationTime(v8::Isolate* isolate) {
  auto timeValue = base::Process::Current().CreationTime();
  if (timeValue.is_null()) {
    return v8::Null(isolate);
  }
  double jsTime = timeValue.ToJsTime();
  return v8::Number::New(isolate, jsTime);
}

// static
v8::Local<v8::Value> ElectronBindings::GetSystemMemoryInfo(
    v8::Isolate* isolate,
    gin_helper::Arguments* args) {
  base::SystemMemoryInfoKB mem_info;
  if (!base::GetSystemMemoryInfo(&mem_info)) {
    args->ThrowError("Unable to retrieve system memory information");
    return v8::Undefined(isolate);
  }

  gin_helper::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
  dict.SetHidden("simple", true);
  dict.Set("total", mem_info.total);

  // See Chromium's "base/process/process_metrics.h" for an explanation.
  int free =
#if defined(OS_WIN)
      mem_info.avail_phys;
#else
      mem_info.free;
#endif
  dict.Set("free", free);

  // NB: These return bogus values on macOS
#if !defined(OS_MACOSX)
  dict.Set("swapTotal", mem_info.swap_total);
  dict.Set("swapFree", mem_info.swap_free);
#endif

  return dict.GetHandle();
}

// static
v8::Local<v8::Promise> ElectronBindings::GetProcessMemoryInfo(
    v8::Isolate* isolate) {
  util::Promise<gin_helper::Dictionary> promise(isolate);
  v8::Local<v8::Promise> handle = promise.GetHandle();

  if (gin_helper::Locker::IsBrowserProcess() && !Browser::Get()->is_ready()) {
    promise.RejectWithErrorMessage(
        "Memory Info is available only after app ready");
    return handle;
  }

  v8::Global<v8::Context> context(isolate, isolate->GetCurrentContext());
  memory_instrumentation::MemoryInstrumentation::GetInstance()
      ->RequestGlobalDumpForPid(
          base::GetCurrentProcId(), std::vector<std::string>(),
          base::BindOnce(&ElectronBindings::DidReceiveMemoryDump,
                         std::move(context), std::move(promise)));
  return handle;
}

// static
v8::Local<v8::Value> ElectronBindings::GetBlinkMemoryInfo(
    v8::Isolate* isolate) {
  auto allocated = blink::ProcessHeap::TotalAllocatedObjectSize();
  auto total = blink::ProcessHeap::TotalAllocatedSpace();

  gin_helper::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
  dict.SetHidden("simple", true);
  dict.Set("allocated", static_cast<double>(allocated >> 10));
  dict.Set("total", static_cast<double>(total >> 10));
  return dict.GetHandle();
}

// static
void ElectronBindings::DidReceiveMemoryDump(
    v8::Global<v8::Context> context,
    util::Promise<gin_helper::Dictionary> promise,
    bool success,
    std::unique_ptr<memory_instrumentation::GlobalMemoryDump> global_dump) {
  v8::Isolate* isolate = promise.isolate();
  gin_helper::Locker locker(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::MicrotasksScope script_scope(isolate,
                                   v8::MicrotasksScope::kRunMicrotasks);
  v8::Context::Scope context_scope(
      v8::Local<v8::Context>::New(isolate, context));

  if (!success) {
    promise.RejectWithErrorMessage("Failed to create memory dump");
    return;
  }

  bool resolved = false;
  for (const memory_instrumentation::GlobalMemoryDump::ProcessDump& dump :
       global_dump->process_dumps()) {
    if (base::GetCurrentProcId() == dump.pid()) {
      gin_helper::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
      const auto& osdump = dump.os_dump();
#if defined(OS_LINUX) || defined(OS_WIN)
      dict.Set("residentSet", osdump.resident_set_kb);
#endif
      dict.Set("private", osdump.private_footprint_kb);
      dict.Set("shared", osdump.shared_footprint_kb);
      promise.ResolveWithGin(dict);
      resolved = true;
      break;
    }
  }
  if (!resolved) {
    promise.RejectWithErrorMessage(
        R"(Failed to find current process memory details in memory dump)");
  }
}

// static
v8::Local<v8::Value> ElectronBindings::GetCPUUsage(
    base::ProcessMetrics* metrics,
    v8::Isolate* isolate) {
  gin_helper::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
  dict.SetHidden("simple", true);
  int processor_count = base::SysInfo::NumberOfProcessors();
  dict.Set("percentCPUUsage",
           metrics->GetPlatformIndependentCPUUsage() / processor_count);

  // NB: This will throw NOTIMPLEMENTED() on Windows
  // For backwards compatibility, we'll return 0
#if !defined(OS_WIN)
  dict.Set("idleWakeupsPerSecond", metrics->GetIdleWakeupsPerSecond());
#else
  dict.Set("idleWakeupsPerSecond", 0);
#endif

  return dict.GetHandle();
}

// static
v8::Local<v8::Value> ElectronBindings::GetIOCounters(v8::Isolate* isolate) {
  auto metrics = base::ProcessMetrics::CreateCurrentProcessMetrics();
  base::IoCounters io_counters;
  gin_helper::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
  dict.SetHidden("simple", true);

  if (metrics->GetIOCounters(&io_counters)) {
    dict.Set("readOperationCount", io_counters.ReadOperationCount);
    dict.Set("writeOperationCount", io_counters.WriteOperationCount);
    dict.Set("otherOperationCount", io_counters.OtherOperationCount);
    dict.Set("readTransferCount", io_counters.ReadTransferCount);
    dict.Set("writeTransferCount", io_counters.WriteTransferCount);
    dict.Set("otherTransferCount", io_counters.OtherTransferCount);
  }

  return dict.GetHandle();
}

// static
bool ElectronBindings::TakeHeapSnapshot(v8::Isolate* isolate,
                                        const base::FilePath& file_path) {
  base::ThreadRestrictions::ScopedAllowIO allow_io;

  base::File file(file_path,
                  base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);

  return electron::TakeHeapSnapshot(isolate, &file);
}

}  // namespace electron
