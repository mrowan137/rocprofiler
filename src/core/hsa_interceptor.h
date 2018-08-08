/******************************************************************************
MIT License

Copyright (c) 2018 ROCm Core Technology

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*******************************************************************************/

#ifndef _SRC_CORE_HSA_INTERCEPTOR_H
#define _SRC_CORE_HSA_INTERCEPTOR_H

#include <hsa.h>
#include <hsa_ext_amd.h>

#include <atomic>
#include <mutex>

#include "inc/rocprofiler.h"
#include "util/exception.h"
#include "util/hsa_rsrc_factory.h"

#define HSA_RT(call) \
  do { \
    const hsa_status_t status = call; \
    if (status != HSA_STATUS_SUCCESS) EXC_ABORT(status, #call); \
  } while(0)

#define IS_HSA_CALLBACK(ID) \
  const auto __id = ID; (void)__id; \
  void *__arg = arg_.load(); (void)__arg; \
  rocprofiler_hsa_callback_fun_t __callback = \
    (ID == ROCPROFILER_HSA_CB_ID_ALLOCATE) ? callbacks_.allocate: \
    (ID == ROCPROFILER_HSA_CB_ID_DEVICE) ? callbacks_.device: \
    (ID == ROCPROFILER_HSA_CB_ID_MEMCOPY) ? callbacks_.memcopy: \
                                            callbacks_.submit; \
  if (__callback != NULL)

#define DO_HSA_CALLBACK \
  do { __callback(__id, &data, __arg); } while (0)

#define ISSUE_HSA_CALLBACK(ID) \
  IS_HSA_CALLBACK(ID) { DO_HSA_CALLBACK; }

namespace rocprofiler {
extern decltype(hsa_memory_allocate)* hsa_memory_allocate_fn;
extern decltype(hsa_memory_assign_agent)* hsa_memory_assign_agent_fn;
extern decltype(hsa_memory_copy)* hsa_memory_copy_fn;
extern decltype(hsa_amd_memory_pool_allocate)* hsa_amd_memory_pool_allocate_fn;
extern decltype(hsa_amd_agents_allow_access)* hsa_amd_agents_allow_access_fn;
extern decltype(hsa_amd_memory_async_copy)* hsa_amd_memory_async_copy_fn;

class HsaInterceptor {
 public:
  typedef std::atomic<void*> arg_t;
  typedef std::mutex mutex_t;

  static void Enable(const bool& enable) { enable_ = enable; }

  static void HsaIntercept(HsaApiTable* table) {
    fprintf(stderr, "HsaInterceptor ...\n");
    if (enable_) {
      fprintf(stderr, "HsaInterceptor enabled\n");
      // saving original API functions
      hsa_memory_allocate_fn = table->core_->hsa_memory_allocate_fn;
      hsa_memory_assign_agent_fn = table->core_->hsa_memory_assign_agent_fn;
      hsa_memory_copy_fn = table->core_->hsa_memory_copy_fn;
      hsa_amd_memory_pool_allocate_fn = table->amd_ext_->hsa_amd_memory_pool_allocate_fn;
      hsa_amd_agents_allow_access_fn = table->amd_ext_->hsa_amd_agents_allow_access_fn;
      hsa_amd_memory_async_copy_fn = table->amd_ext_->hsa_amd_memory_async_copy_fn;
      // intercepting API
      table->core_->hsa_memory_allocate_fn = MemoryAllocate;
      table->core_->hsa_memory_assign_agent_fn = MemoryAssignAgent;
      table->core_->hsa_memory_copy_fn = MemoryCopy;
      table->amd_ext_->hsa_amd_memory_pool_allocate_fn = MemoryPoolAllocate;
      table->amd_ext_->hsa_amd_agents_allow_access_fn = AgentsAllowAccess;
      table->amd_ext_->hsa_amd_memory_async_copy_fn = MemoryAsyncCopy;
    }
    fprintf(stderr, "HsaInterceptor done\n");
  }

  static void SetCallbacks(rocprofiler_hsa_callbacks_t callbacks, void* arg) {
    std::lock_guard<mutex_t> lck(mutex_);
    callbacks_ = callbacks;
    arg_.store(arg);
  }

 private:
  static hsa_status_t HSA_API MemoryAllocate(hsa_region_t region,
    size_t size,
    void** ptr)
  {
    EXC_ABORT(HSA_STATUS_ERROR, "Deprecated API");
    hsa_status_t status = HSA_STATUS_SUCCESS;
    HSA_RT(hsa_memory_allocate_fn(region, size, ptr));
    IS_HSA_CALLBACK(ROCPROFILER_HSA_CB_ID_ALLOCATE) {
      rocprofiler_hsa_callback_data_t data{};
      data.allocate.addr = *ptr;
      data.allocate.size = size;

      HSA_RT(hsa_region_get_info(region, HSA_REGION_INFO_SEGMENT, &data.allocate.segment));
      HSA_RT(hsa_region_get_info(region, HSA_REGION_INFO_GLOBAL_FLAGS, &data.allocate.global_flag));

      DO_HSA_CALLBACK;
    }
    return status;
  }

  static hsa_status_t MemoryAssignAgent(
    void *ptr,
    hsa_agent_t agent,
    hsa_access_permission_t access)
  {
    EXC_ABORT(HSA_STATUS_ERROR, "Deprecated API");
    hsa_status_t status = HSA_STATUS_SUCCESS;
    HSA_RT(hsa_memory_assign_agent_fn(ptr, agent, access));
    IS_HSA_CALLBACK(ROCPROFILER_HSA_CB_ID_DEVICE) {
      rocprofiler_hsa_callback_data_t data{};
      data.device.mem = ptr;

      HSA_RT(hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &data.device.type));

      DO_HSA_CALLBACK;
    }
    return status;
  }

  // Spawn device allow access callback
  static void DeviceCallback(
    uint32_t num_agents,
    const hsa_agent_t* agents,
    const void* ptr)
  {
    for (const hsa_agent_t* agent_p = agents; agent_p < (agents + num_agents); ++agent_p) {
      hsa_agent_t agent = *agent_p;
      rocprofiler_hsa_callback_data_t data{};
      data.device.id = util::HsaRsrcFactory::Instance().GetAgentInfo(agent)->dev_index;
      data.device.mem = ptr;

      HSA_RT(hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &data.device.type));

      ISSUE_HSA_CALLBACK(ROCPROFILER_HSA_CB_ID_DEVICE);
    }
  }

  // Agent allow access callback 'hsa_amd_agents_allow_access'
  static hsa_status_t AgentsAllowAccess(
    uint32_t num_agents,
    const hsa_agent_t* agents,
    const uint32_t* flags,
    const void* ptr)
  {
    hsa_status_t status = HSA_STATUS_SUCCESS;
    HSA_RT(hsa_amd_agents_allow_access_fn(num_agents, agents, flags, ptr));
    IS_HSA_CALLBACK(ROCPROFILER_HSA_CB_ID_DEVICE) {
      DeviceCallback(num_agents, agents, ptr);
    }
    return status;
  }

  // Callback function to get available in the system agents
  struct agent_callback_data_t {
    hsa_amd_memory_pool_t pool;
    void* addr;
  };
  static hsa_status_t AgentCallback(hsa_agent_t agent, void* data) {
    agent_callback_data_t* callback_data = reinterpret_cast<agent_callback_data_t*>(data);
    hsa_amd_agent_memory_pool_info_t attribute = HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS;
    hsa_amd_memory_pool_access_t value;
    HSA_RT(hsa_amd_agent_memory_pool_get_info(agent, callback_data->pool, attribute, &value));
    if (value == HSA_AMD_MEMORY_POOL_ACCESS_ALLOWED_BY_DEFAULT) {
      DeviceCallback(1, &agent, callback_data->addr);
    }
    return HSA_STATUS_SUCCESS;
  }

  static hsa_status_t MemoryPoolAllocate(
    hsa_amd_memory_pool_t pool,
    size_t size,
    uint32_t flags,
    void** ptr)
  {
    hsa_status_t status = HSA_STATUS_SUCCESS;
    HSA_RT(hsa_amd_memory_pool_allocate_fn(pool, size, flags, ptr));
    IS_HSA_CALLBACK(ROCPROFILER_HSA_CB_ID_ALLOCATE) {
      rocprofiler_hsa_callback_data_t data{};
      data.allocate.addr = *ptr;
      data.allocate.size = size;

      HSA_RT(hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &data.allocate.segment));
      HSA_RT(hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &data.allocate.global_flag));
#if 0
      HSA_RT(hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_ACCESSIBLE_BY_ALL, &data.allocate.global_mem));
#endif

      DO_HSA_CALLBACK;

      IS_HSA_CALLBACK(ROCPROFILER_HSA_CB_ID_DEVICE) {
        // Scan the pool assigned devices
        agent_callback_data_t callback_data{pool, *ptr};
        hsa_iterate_agents(AgentCallback, &callback_data);
      }
    }
    return status;
  }

  static hsa_status_t MemoryCopy(
    void *dst,
    const void *src,
    size_t size)
  {
    hsa_status_t status = HSA_STATUS_SUCCESS;
    HSA_RT(hsa_memory_copy_fn(dst, src, size));
    IS_HSA_CALLBACK(ROCPROFILER_HSA_CB_ID_MEMCOPY) {
      rocprofiler_hsa_callback_data_t data{};
      data.memcopy.dst = dst;
      data.memcopy.src = src;
      data.memcopy.size = size;
      DO_HSA_CALLBACK;
    }
    return status;
  }

  static hsa_status_t MemoryAsyncCopy(
    void* dst, hsa_agent_t dst_agent, const void* src,
    hsa_agent_t src_agent, size_t size,
    uint32_t num_dep_signals,
    const hsa_signal_t* dep_signals,
    hsa_signal_t completion_signal)
  {
    hsa_status_t status = HSA_STATUS_SUCCESS;
    HSA_RT(hsa_amd_memory_async_copy_fn(
      dst, dst_agent, src, src_agent, size,
      num_dep_signals, dep_signals, completion_signal));
    IS_HSA_CALLBACK(ROCPROFILER_HSA_CB_ID_MEMCOPY) {
      rocprofiler_hsa_callback_data_t data{};
      data.memcopy.dst = dst;
      data.memcopy.src = src;
      data.memcopy.size = size;
      DO_HSA_CALLBACK;
    }
    return status;
  }

  static bool enable_;
  static rocprofiler_hsa_callbacks_t callbacks_;
  static arg_t arg_;
  static mutex_t mutex_;
};

}  // namespace rocprofiler

#endif  // _SRC_CORE_HSA_INTERCEPTOR_H
