/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CuptiActivityProfiler.h"
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "ApproximateClock.h"

#ifdef HAS_CUPTI
#include <cupti.h>
#elif defined(HAS_ROCTRACER)
#include <roctracer.h>
#endif

#include "Config.h"
#include "DeviceProperties.h"
#include "DeviceUtil.h"
#include "time_since_epoch.h"
#ifdef HAS_CUPTI
#include "CuptiActivity.cpp"
#include "CuptiActivity.h"
#include "CuptiActivityApi.h"
#include "KernelRegistry.h"
#endif // HAS_CUPTI
#ifdef HAS_ROCTRACER
#include "RoctracerActivity.h"
#include "RoctracerActivityApi.h"
#include "RoctracerLogger.h"
#endif
#ifdef HAS_XPUPTI
#include "plugin/xpupti/XpuptiActivityProfiler.h"
#endif
#include "ActivityBuffers.h"
#include "output_base.h"

#include "Logger.h"
#include "ThreadUtil.h"

using namespace std::chrono;
using std::string;

struct CtxEventPair {
  uint32_t ctx = 0;
  uint32_t eventId = 0;

  bool operator==(const CtxEventPair& other) const {
    return (this->ctx == other.ctx) && (this->eventId == other.eventId);
  }
};

template <>
struct std::hash<CtxEventPair> {
  std::size_t operator()(const CtxEventPair& c) const {
    return KINETO_NAMESPACE::detail::hash_combine(
        std::hash<uint32_t>()(c.ctx), std::hash<uint32_t>()(c.eventId));
  }
};

struct WaitEventInfo {
  // CUDA stream that the CUDA event was recorded on
  uint32_t stream;
  // Correlation ID of the cudaEventRecord event
  uint32_t correlationId;
};

namespace {

// Map (ctx, eventId) -> (stream, corr Id) that recorded the CUDA event
std::unordered_map<CtxEventPair, WaitEventInfo>& waitEventMap() {
  static std::unordered_map<CtxEventPair, WaitEventInfo> waitEventMap_;
  return waitEventMap_;
}

// Map ctx -> deviceId
std::unordered_map<uint32_t, uint32_t>& ctxToDeviceId() {
  static std::unordered_map<uint32_t, uint32_t> ctxToDeviceId_;
  return ctxToDeviceId_;
}

} // namespace

namespace KINETO_NAMESPACE {

#ifdef HAS_CUPTI
bool& use_cupti_tsc() {
  static bool use_cupti_tsc = true;
  return use_cupti_tsc;
}
#endif

ConfigDerivedState::ConfigDerivedState(const Config& config) {
  profileActivityTypes_ = config.selectedActivityTypes();
  profileStartTime_ = config.requestTimestamp();
  profileDuration_ = config.activitiesDuration();
  profileWarmupDuration_ = config.activitiesWarmupDuration();
  profilingByIter_ = config.hasProfileStartIteration();
  perThreadBufferEnabled_ = config.perThreadBufferEnabled();
  if (profilingByIter_) {
    profileStartIter_ = config.profileStartIteration();
    profileEndIter_ = profileStartIter_ + config.activitiesRunIterations();
  } else {
    profileEndIter_ = (std::numeric_limits<decltype(profileEndIter_)>::max)();
    profileEndTime_ = profileStartTime_ + config.activitiesDuration();
  }
}

bool ConfigDerivedState::canStart(
    const std::chrono::time_point<std::chrono::system_clock>& now) const {
  if (profilingByIter_) {
    return true;
  }
  if (profileStartTime_ < now) {
    LOG(ERROR)
        << "Not starting tracing - start timestamp is in the past. Time difference (ms): "
        << duration_cast<milliseconds>(now - profileStartTime_).count();
    return false;
  } else if ((profileStartTime_ - now) < profileWarmupDuration_) {
    LOG(ERROR)
        << "Not starting tracing - insufficient time for warmup. Time to warmup (ms): "
        << duration_cast<milliseconds>(profileStartTime_ - now).count();
    return false;
  }
  return true;
}

bool ConfigDerivedState::isWarmupDone(
    const time_point<system_clock>& now,
    int64_t currentIter) const {
  bool isTimestampBased = !profilingByIter_ && currentIter < 0;
  if (isTimestampBased) {
    // qualify that this check is not being called from application step() API
    // this avoids races between the step() API and periodically invoked
    // profiler run loop step() method
    return now >= profileStartTime_;
  }
  bool isIterationBased = profilingByIter_ && currentIter >= 0;
  if (isIterationBased) {
    return currentIter >= profileStartIter_;
  }
  return false;
}

bool ConfigDerivedState::isCollectionDone(
    const time_point<system_clock>& now,
    int64_t currentIter) const {
  bool isTimestampBased = !profilingByIter_ && currentIter < 0;
  if (isTimestampBased) {
    // qualify that this check is not being called from application step() API
    return now >= profileEndTime_;
  }
  bool isIterationBased = profilingByIter_ && currentIter >= 0;
  if (isIterationBased) {
    return currentIter >= profileEndIter_;
  }
  return false;
}

std::ostream& operator<<(
    std::ostream& oss,
    const CuptiActivityProfiler::ErrorCounts& ecs) {
  oss << "Out-of-range = " << ecs.out_of_range_events
      << ", Blocklisted runtime = " << ecs.blocklisted_runtime_events
      << ", Invalid ext correlations = "
      << ecs.invalid_external_correlation_events
      << ", CPU GPU out-of-order = " << ecs.gpu_and_cpu_op_out_of_order
#if defined(HAS_CUPTI) || defined(HAS_ROCTRACER)
      << ", Unexpected CUDA events = " << ecs.unexepected_cuda_events
      << ", CUPTI stopped early? = " << ecs.cupti_stopped_early;
#else
      ;
#endif // HAS_CUPTI || HAS_ROCTRACER
  return oss;
}

CuptiActivityProfiler::~CuptiActivityProfiler() {
  if (collectTraceThread_ && collectTraceThread_->joinable()) {
    collectTraceThread_->join();
  }
}

void CuptiActivityProfiler::transferCpuTrace(
    std::unique_ptr<libkineto::CpuTraceBuffer> cpuTrace) {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  const string& trace_name = cpuTrace->span.name;
  if (currentRunloopState_ != RunloopState::CollectTrace &&
      currentRunloopState_ != RunloopState::ProcessTrace) {
    VLOG(0) << "Trace collection not in progress - discarding span "
            << trace_name;
    return;
  }

  cpuTrace->span.iteration = iterationCountMap_[trace_name]++;

  VLOG(0) << "Received iteration " << cpuTrace->span.iteration << " of span "
          << trace_name << " (" << cpuTrace->activities.size()
          << " activities / " << cpuTrace->gpuOpCount << " gpu activities)";
  traceBuffers_->cpu.push_back(std::move(cpuTrace));
}

#ifdef HAS_ROCTRACER
CuptiActivityProfiler::CuptiActivityProfiler(
    RoctracerActivityApi& cupti,
    bool cpuOnly)
#else
CuptiActivityProfiler::CuptiActivityProfiler(
    CuptiActivityApi& cupti,
    bool cpuOnly)
#endif
    : cupti_(cupti),
      flushOverhead_{0, 0},
      setupOverhead_{0, 0},
      cpuOnly_{cpuOnly},
      currentRunloopState_{RunloopState::WaitForRequest} {

  if (isGpuAvailable()) {
    logGpuVersions();
  }
}

void CuptiActivityProfiler::logGpuVersions() {
#ifdef HAS_CUPTI
  // check Nvidia versions
  uint32_t cuptiVersion = 0;
  int cudaRuntimeVersion = 0, cudaDriverVersion = 0;
  CUPTI_CALL(cuptiGetVersion(&cuptiVersion));
  CUDA_CALL(cudaRuntimeGetVersion(&cudaRuntimeVersion));
  CUDA_CALL(cudaDriverGetVersion(&cudaDriverVersion));
  LOG(INFO) << "CUDA versions. CUPTI: " << cuptiVersion
            << "; Runtime: " << cudaRuntimeVersion
            << "; Driver: " << cudaDriverVersion;

  LOGGER_OBSERVER_ADD_METADATA("cupti_version", std::to_string(cuptiVersion));
  LOGGER_OBSERVER_ADD_METADATA(
      "cuda_runtime_version", std::to_string(cudaRuntimeVersion));
  LOGGER_OBSERVER_ADD_METADATA(
      "cuda_driver_version", std::to_string(cudaDriverVersion));
  addVersionMetadata("cupti_version", std::to_string(cuptiVersion));
  addVersionMetadata(
      "cuda_runtime_version", std::to_string(cudaRuntimeVersion));
  addVersionMetadata("cuda_driver_version", std::to_string(cudaDriverVersion));

#elif defined(HAS_ROCTRACER)
  uint32_t majorVersion = roctracer_version_major();
  uint32_t minorVersion = roctracer_version_minor();
  std::string roctracerVersion =
      std::to_string(majorVersion) + "." + std::to_string(minorVersion);
  int hipRuntimeVersion = 0, hipDriverVersion = 0;
  CUDA_CALL(hipRuntimeGetVersion(&hipRuntimeVersion));
  CUDA_CALL(hipDriverGetVersion(&hipDriverVersion));
  LOG(INFO) << "HIP versions. Roctracer: " << roctracerVersion
            << "; Runtime: " << hipRuntimeVersion
            << "; Driver: " << hipDriverVersion;

  LOGGER_OBSERVER_ADD_METADATA("roctracer_version", roctracerVersion);
  LOGGER_OBSERVER_ADD_METADATA(
      "hip_runtime_version", std::to_string(hipRuntimeVersion));
  LOGGER_OBSERVER_ADD_METADATA(
      "hip_driver_version", std::to_string(hipDriverVersion));
  addVersionMetadata("roctracer_version", roctracerVersion);
  addVersionMetadata("hip_runtime_version", std::to_string(hipRuntimeVersion));
  addVersionMetadata("hip_driver_version", std::to_string(hipDriverVersion));

#endif
}

namespace {

const std::unordered_set<std::string>& getLoggerMedataAllowList() {
  static const std::unordered_set<std::string> kLoggerMedataAllowList{
      "with_stack", "with_modules", "record_shapes", "profile_memory"};
  return kLoggerMedataAllowList;
}

} // namespace

void CuptiActivityProfiler::processTraceInternal(ActivityLogger& logger) {
  LOG(INFO) << "Processing " << traceBuffers_->cpu.size() << " CPU buffers";
  VLOG(0) << "Profile time range: " << captureWindowStartTime_ << " - "
          << captureWindowEndTime_;

  // Pass metadata within the trace to the logger observer.
  for (const auto& pair : metadata_) {
    if (getLoggerMedataAllowList().contains(pair.first)) {
      LOGGER_OBSERVER_ADD_METADATA(pair.first, pair.second);
    }
  }
  for (auto& pair : versionMetadata_) {
    addMetadata(pair.first, pair.second);
  }
  std::vector<std::string> device_properties;
  if (const auto& props = devicePropertiesJson(); !props.empty()) {
    device_properties.push_back(props);
  }
  for (const auto& session : sessions_) {
    if (auto props = session->getDeviceProperties(); !props.empty()) {
      if (std::find(
              device_properties.begin(), device_properties.end(), props) ==
          device_properties.end()) {
        device_properties.push_back(props);
      }
    }
    for (const auto& [key, value] : session->getMetadata()) {
      addMetadata(key, value);
    }
  }
  logger.handleTraceStart(
      metadata_, fmt::format("{}", fmt::join(device_properties, ",")));
  setCpuActivityPresent(false);
  setGpuActivityPresent(false);
  for (auto& cpu_trace : traceBuffers_->cpu) {
    string trace_name = cpu_trace->span.name;
    VLOG(0) << "Processing CPU buffer for " << trace_name << " ("
            << cpu_trace->span.iteration << ") - "
            << cpu_trace->activities.size() << " records";
    VLOG(0) << "Span time range: " << cpu_trace->span.startTime << " - "
            << cpu_trace->span.endTime;
    processCpuTrace(*cpu_trace, logger);
    LOGGER_OBSERVER_ADD_EVENT_COUNT(cpu_trace->activities.size());
  }

#ifdef HAS_CUPTI
  if (!cpuOnly_) {
    VLOG(0) << "Retrieving GPU activity buffers";
    traceBuffers_->gpu = cupti_.activityBuffers();
    if (VLOG_IS_ON(1)) {
      addOverheadSample(flushOverhead_, cupti_.flushOverhead);
    }
    if (traceBuffers_->gpu) {
      const auto count_and_size = cupti_.processActivities(
          *traceBuffers_->gpu,
          std::bind(
              &CuptiActivityProfiler::handleCuptiActivity,
              this,
              std::placeholders::_1,
              &logger));
      logDeferredEvents();
      LOG(INFO) << "Processed " << count_and_size.first << " GPU records ("
                << count_and_size.second << " bytes)";
      LOGGER_OBSERVER_ADD_EVENT_COUNT(count_and_size.first);

      // resourceOverheadCount_ is set while processing GPU activities
      if (resourceOverheadCount_ > 0) {
        LOG(INFO) << "Allocated " << resourceOverheadCount_
                  << " extra CUPTI buffers.";
      }
      LOGGER_OBSERVER_ADD_METADATA(
          "ResourceOverhead", std::to_string(resourceOverheadCount_));
    }
    if (!gpuActivityPresent()) {
      LOG(WARNING) << "GPU trace is empty!";
    }
  }
#endif // HAS_CUPTI
#ifdef HAS_ROCTRACER
  if (!cpuOnly_) {
    VLOG(0) << "Retrieving GPU activity buffers";
    const int count = cupti_.processActivities(
        std::bind(
            &CuptiActivityProfiler::handleRoctracerActivity,
            this,
            std::placeholders::_1,
            &logger),
        std::bind(
            &CuptiActivityProfiler::handleCorrelationActivity,
            this,
            std::placeholders::_1,
            std::placeholders::_2,
            std::placeholders::_3));
    LOG(INFO) << "Processed " << count << " GPU records";
    LOGGER_OBSERVER_ADD_EVENT_COUNT(count);
  }
#endif // HAS_ROCTRACER
  if (!traceNonEmpty()) {
    LOG(WARNING) << kEmptyTrace;
  }

  for (const auto& session : sessions_) {
    LOG(INFO) << "Processing child profiler trace";
    // cpuActivity() function here is used to get the linked cpuActivity for
    // session's activities. Passing captureWindowStartTime_ and
    // captureWindowEndTime_ in order to specify the range of activities that
    // need to be processed.
    session->processTrace(
        logger,
        std::bind(
            &CuptiActivityProfiler::cpuActivity, this, std::placeholders::_1),
        captureWindowStartTime_,
        captureWindowEndTime_);
  }

  LOG(INFO) << "Record counts: " << ecs_;

  finalizeTrace(*config_, logger);
}

CuptiActivityProfiler::CpuGpuSpanPair& CuptiActivityProfiler::recordTraceSpan(
    TraceSpan& span,
    int gpuOpCount) {
  TraceSpan gpu_span(gpuOpCount, span.iteration, span.name, "GPU: ");
  auto& iterations = traceSpans_[span.name];
  iterations.emplace_back(span, gpu_span);
  return iterations.back();
}

void CuptiActivityProfiler::processCpuTrace(
    libkineto::CpuTraceBuffer& cpuTrace,
    ActivityLogger& logger) {
  if (cpuTrace.activities.empty()) {
    LOG(WARNING) << "CPU trace is empty!";
    return;
  }
  setCpuActivityPresent(true);
  bool warn_once = false;
  CpuGpuSpanPair& span_pair =
      recordTraceSpan(cpuTrace.span, cpuTrace.gpuOpCount);
  TraceSpan& cpu_span = span_pair.first;
  for (auto const& act : cpuTrace.activities) {
    VLOG(2) << act->correlationId() << ": OP " << act->activityName;
    if (derivedConfig_->profileActivityTypes().contains(act->type())) {
      static_assert(
          std::is_same_v<
              std::remove_reference_t<decltype(act)>,
              const std::unique_ptr<GenericTraceActivity>>,
          "handleActivity is unsafe and relies on the caller to maintain not "
          "only lifetime but also address stability.");
      if (act->duration() < 0) {
        act->endTime = captureWindowEndTime_;
        act->addMetadata("finished", "false");
      }
      logger.handleActivity(*act);
    }
    clientActivityTraceMap_[act->correlationId()] = &span_pair;
    activityMap_[act->correlationId()] = act.get();
    if (act->deviceId() == 0) {
      if (!warn_once) {
        LOG(WARNING)
            << "CPU activity with pid 0 detected. This is likely due to the python stack"
               " tracer not being able to determine the pid for an event. Overriding pid to main thread pid";
      }
      act->setDevice(processId());
      warn_once = true;
    }
    recordThreadInfo(act->resourceId(), act->getThreadId(), act->deviceId());
  }
  logger.handleTraceSpan(cpu_span);
}

#ifdef HAS_CUPTI
inline void CuptiActivityProfiler::handleCorrelationActivity(
    const CUpti_ActivityExternalCorrelation* correlation) {
  if (correlation->externalKind == CUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM0) {
    cpuCorrelationMap_[correlation->correlationId] = correlation->externalId;
  } else if (
      correlation->externalKind == CUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM1) {
    userCorrelationMap_[correlation->correlationId] = correlation->externalId;
  } else {
    LOG(WARNING)
        << "Invalid CUpti_ActivityExternalCorrelation sent to handleCuptiActivity";
    ecs_.invalid_external_correlation_events++;
  }
}
#endif // HAS_CUPTI
#ifdef HAS_ROCTRACER
inline void CuptiActivityProfiler::handleCorrelationActivity(
    uint64_t correlationId,
    uint64_t externalId,
    RoctracerLogger::CorrelationDomain externalKind) {
  if (externalKind == RoctracerLogger::CorrelationDomain::Domain0) {
    cpuCorrelationMap_[correlationId] = externalId;
  } else if (externalKind == RoctracerLogger::CorrelationDomain::Domain1) {
    userCorrelationMap_[correlationId] = externalId;
  } else {
    LOG(WARNING)
        << "Invalid CUpti_ActivityExternalCorrelation sent to handleCuptiActivity";
    ecs_.invalid_external_correlation_events++;
  }
}
#endif // HAS_ROCTRACER

static GenericTraceActivity createUserGpuSpan(
    const libkineto::ITraceActivity& cpuTraceActivity,
    const libkineto::ITraceActivity& gpuTraceActivity) {
  GenericTraceActivity res(
      *cpuTraceActivity.traceSpan(),
      ActivityType::GPU_USER_ANNOTATION,
      cpuTraceActivity.name());
  res.startTime = gpuTraceActivity.timestamp();
  res.device = gpuTraceActivity.deviceId();
  res.resource = gpuTraceActivity.resourceId();
  res.endTime = gpuTraceActivity.timestamp() + gpuTraceActivity.duration();
  res.id = cpuTraceActivity.correlationId();
  return res;
}

void CuptiActivityProfiler::GpuUserEventMap::insertOrExtendEvent(
    const ITraceActivity& cpuTraceActivity,
    const ITraceActivity& gpuTraceActivity) {
  StreamKey key(gpuTraceActivity.deviceId(), gpuTraceActivity.resourceId());
  CorrelationSpanMap& correlationSpanMap = streamSpanMap_[key];
  auto it = correlationSpanMap.find(cpuTraceActivity.correlationId());
  if (it == correlationSpanMap.end()) {
    auto it_success = correlationSpanMap.insert(
        {cpuTraceActivity.correlationId(),
         createUserGpuSpan(cpuTraceActivity, gpuTraceActivity)});
    it = it_success.first;
  }
  GenericTraceActivity& span = it->second;
  if (gpuTraceActivity.timestamp() < span.startTime || span.startTime == 0) {
    span.startTime = gpuTraceActivity.timestamp();
  }
  int64_t gpu_activity_end =
      gpuTraceActivity.timestamp() + gpuTraceActivity.duration();
  span.endTime = std::max(gpu_activity_end, span.endTime);
}

const CuptiActivityProfiler::CpuGpuSpanPair&
CuptiActivityProfiler::defaultTraceSpan() {
  static TraceSpan span(0, 0, "Unknown", "");
  static CpuGpuSpanPair span_pair(span, span);
  return span_pair;
}

void CuptiActivityProfiler::GpuUserEventMap::logEvents(ActivityLogger* logger) {
  for (auto const& streamMapPair : streamSpanMap_) {
    for (auto const& correlationSpanPair : streamMapPair.second) {
      correlationSpanPair.second.log(*logger);
    }
  }
}

inline bool CuptiActivityProfiler::outOfRange(const ITraceActivity& act) {
  bool out_of_range = act.timestamp() < captureWindowStartTime_ ||
      (act.timestamp() + act.duration()) > captureWindowEndTime_;
  if (out_of_range) {
    VLOG(2) << "TraceActivity outside of profiling window: " << act.name()
            << " (" << act.timestamp() << " < " << captureWindowStartTime_
            << " or " << (act.timestamp() + act.duration()) << " > "
            << captureWindowEndTime_;
    ecs_.out_of_range_events++;
  }
  // Range Profiling mode returns kernels with 0 ts and duration that we can
  // pass through to output
  bool zero_ts = rangeProfilingActive_ && (act.timestamp() == 0);
  return !zero_ts && out_of_range;
}

#ifdef HAS_CUPTI
inline static bool isBlockListedRuntimeCbid(CUpti_CallbackId cbid) {
  // Some CUDA calls that are very frequent and also not very interesting.
  // Filter these out to reduce trace size.
  if (cbid == CUPTI_RUNTIME_TRACE_CBID_cudaGetDevice_v3020 ||
      cbid == CUPTI_RUNTIME_TRACE_CBID_cudaSetDevice_v3020 ||
      cbid == CUPTI_RUNTIME_TRACE_CBID_cudaGetLastError_v3020 ||
      // Support cudaEventRecord and cudaEventSynchronize, revisit if others
      // are needed
      cbid == CUPTI_RUNTIME_TRACE_CBID_cudaEventCreate_v3020 ||
      cbid == CUPTI_RUNTIME_TRACE_CBID_cudaEventCreateWithFlags_v3020 ||
      cbid == CUPTI_RUNTIME_TRACE_CBID_cudaEventDestroy_v3020) {
    return true;
  }

  return false;
}

void CuptiActivityProfiler::handleRuntimeActivity(
    const CUpti_ActivityAPI* activity,
    ActivityLogger* logger) {
  if (isBlockListedRuntimeCbid(activity->cbid)) {
    ecs_.blocklisted_runtime_events++;
    return;
  }
  VLOG(2) << activity->correlationId
          << ": CUPTI_ACTIVITY_KIND_RUNTIME, cbid=" << activity->cbid
          << " tid=" << activity->threadId;
  int32_t tid = activity->threadId;
  const auto& it = resourceInfo_.find({processId(), tid});
  if (it != resourceInfo_.end()) {
    tid = it->second.id;
  }
  const ITraceActivity* linked =
      linkedActivity(activity->correlationId, cpuCorrelationMap_);
  const auto& runtime_activity =
      traceBuffers_->addActivityWrapper(RuntimeActivity(activity, linked, tid));
  checkTimestampOrder(&runtime_activity);
  if (outOfRange(runtime_activity)) {
    return;
  }
  runtime_activity.log(*logger);
  setGpuActivityPresent(true);
}

void CuptiActivityProfiler::handleDriverActivity(
    const CUpti_ActivityAPI* activity,
    ActivityLogger* logger) {
  // we only want to collect cuLaunchKernel events, for triton kernel launches
  if (!isKernelLaunchApi(*activity)) {
    // XXX should we count other driver events?
    return;
  }
  VLOG(2) << activity->correlationId
          << ": CUPTI_ACTIVITY_KIND_DRIVER, cbid=" << activity->cbid
          << " tid=" << activity->threadId;
  int32_t tid = activity->threadId;
  const auto& it = resourceInfo_.find({processId(), tid});
  if (it != resourceInfo_.end()) {
    tid = it->second.id;
  }
  const ITraceActivity* linked =
      linkedActivity(activity->correlationId, cpuCorrelationMap_);
  const auto& runtime_activity =
      traceBuffers_->addActivityWrapper(DriverActivity(activity, linked, tid));
  checkTimestampOrder(&runtime_activity);
  if (outOfRange(runtime_activity)) {
    return;
  }
  runtime_activity.log(*logger);
  setGpuActivityPresent(true);
}

void CuptiActivityProfiler::handleOverheadActivity(
    const CUpti_ActivityOverhead* activity,
    ActivityLogger* logger) {
  VLOG(2) << ": CUPTI_ACTIVITY_KIND_OVERHEAD"
          << " overheadKind=" << activity->overheadKind;
  const auto& overhead_activity =
      traceBuffers_->addActivityWrapper(OverheadActivity(activity, nullptr));
  // Monitor memory overhead
  if (activity->overheadKind == CUPTI_ACTIVITY_OVERHEAD_CUPTI_RESOURCE) {
    resourceOverheadCount_++;
  }

  if (outOfRange(overhead_activity)) {
    return;
  }
  overhead_activity.log(*logger);
  setGpuActivityPresent(true);
}

static std::optional<WaitEventInfo> getWaitEventInfo(
    uint32_t ctx,
    uint32_t eventId) {
  auto key = CtxEventPair{ctx, eventId};
  auto it = waitEventMap().find(key);
  if (it != waitEventMap().end()) {
    return it->second;
  }
  return std::nullopt;
}

void CuptiActivityProfiler::handleCudaEventActivity(
    const CUpti_ActivityCudaEvent* activity) {
  VLOG(2) << ": CUPTI_ACTIVITY_KIND_CUDA_EVENT"
          << " corrId=" << activity->correlationId
          << " eventId=" << activity->eventId
          << " streamId=" << activity->streamId
          << " contextId=" << activity->contextId;

  // Update the stream, corrID the cudaEvent was last recorded on
  auto key = CtxEventPair{activity->contextId, activity->eventId};
  waitEventMap()[key] =
      WaitEventInfo{activity->streamId, activity->correlationId};
}

void CuptiActivityProfiler::handleCudaSyncActivity(
    const CUpti_ActivitySynchronization* activity,
    ActivityLogger* logger) {
  VLOG(2) << ": CUPTI_ACTIVITY_KIND_SYNCHRONIZATION"
          << " type=" << syncTypeString(activity->type)
          << " corrId=" << activity->correlationId
          << " streamId=" << activity->streamId
          << " eventId=" << activity->cudaEventId
          << " contextId=" << activity->contextId;

  if (!config_->activitiesCudaSyncWaitEvents() &&
      isWaitEventSync(activity->type)) {
    return;
  }

  auto device_id = contextIdtoDeviceId(activity->contextId);
  int32_t src_stream = -1, src_corrid = -1;

  if (isEventSync(activity->type)) {
    auto maybe_wait_event_info =
        getWaitEventInfo(activity->contextId, activity->cudaEventId);
    if (maybe_wait_event_info) {
      src_stream = maybe_wait_event_info->stream;
      src_corrid = maybe_wait_event_info->correlationId;
    }
  }

  // Marshal the logging to a functor so we can defer it if needed.
  auto log_event = [=]() {
    const ITraceActivity* linked =
        linkedActivity(activity->correlationId, cpuCorrelationMap_);
    const auto& cuda_sync_activity = traceBuffers_->addActivityWrapper(
        CudaSyncActivity(activity, linked, src_stream, src_corrid));

    if (outOfRange(cuda_sync_activity)) {
      return;
    }

    if (int32_t(activity->streamId) != -1) {
      recordStream(device_id, activity->streamId, "");
    } else {
      recordDevice(device_id);
    }
    VLOG(2) << "Logging sync event device = " << device_id
            << " stream = " << activity->streamId
            << " sync type = " << syncTypeString(activity->type);
    cuda_sync_activity.log(*logger);
    setGpuActivityPresent(true);
  };

  if (isWaitEventSync(activity->type)) {
    // Defer logging wait event syncs till the end so we only
    // log these events if a stream has some GPU kernels on it.
    DeferredLogEntry entry;
    entry.device = device_id;
    entry.stream = activity->streamId;
    entry.logMe = log_event;

    logQueue_.push_back(entry);
  } else {
    log_event();
  }
}

void CuptiActivityProfiler::logDeferredEvents() {
  // Stream Wait Events tend to be noisy, only pass these events if
  // there was some GPU kernel/memcopy/memset observed on it in the trace
  // window.
  for (const auto& entry : logQueue_) {
    if (seenDeviceStreams_.find({entry.device, entry.stream}) ==
        seenDeviceStreams_.end()) {
      VLOG(2) << "Skipping Event Sync as no kernels have run yet on stream = "
              << entry.stream;
    } else {
      entry.logMe();
    }
  }
}
#endif // HAS_CUPTI

inline void CuptiActivityProfiler::updateGpuNetSpan(
    const ITraceActivity& gpuOp) {
  if (!gpuOp.linkedActivity()) {
    VLOG(0) << "Missing linked activity";
    return;
  }
  const auto& it =
      clientActivityTraceMap_.find(gpuOp.linkedActivity()->correlationId());
  if (it == clientActivityTraceMap_.end()) {
    // No correlation id mapping?
    return;
  }
  TraceSpan& gpu_span = it->second->second;
  if (gpuOp.timestamp() < gpu_span.startTime || gpu_span.startTime == 0) {
    gpu_span.startTime = gpuOp.timestamp();
  }
  gpu_span.endTime =
      std::max(gpuOp.timestamp() + gpuOp.duration(), gpu_span.endTime);
}

// I've observed occasional broken timestamps attached to GPU events...
void CuptiActivityProfiler::checkTimestampOrder(const ITraceActivity* act1) {
  // Correlated GPU runtime activity cannot
  // have timestamp greater than the GPU activity's
  const auto& it = correlatedCudaActivities_.find(act1->correlationId());
  if (it == correlatedCudaActivities_.end()) {
    correlatedCudaActivities_.insert({act1->correlationId(), act1});
    return;
  }

  // Activities may be appear in the buffers out of order.
  // If we have a runtime activity in the map, it should mean that we
  // have a GPU activity passed in, and vice versa.
  const ITraceActivity* act2 = it->second;
  if (act2->type() == ActivityType::CUDA_RUNTIME) {
    // Buffer is out-of-order.
    // Swap so that runtime activity is first for the comparison below.
    std::swap(act1, act2);
  }
  // Range Profiling mode returns kernels with 0 ts and duration that we can
  // pass through to output
  if (act2->timestamp() == 0) {
    return;
  }
  if (act1->timestamp() > act2->timestamp()) {
    LOG_FIRST_N(WARNING, 10)
        << "GPU op timestamp (" << act2->timestamp()
        << ") < runtime timestamp (" << act1->timestamp() << ") by "
        << act1->timestamp() - act2->timestamp() << "us"
        << " Name: " << act2->name() << " Device: " << act2->deviceId()
        << " Stream: " << act2->resourceId();
    ecs_.gpu_and_cpu_op_out_of_order++;
  }
}

const ITraceActivity* CuptiActivityProfiler::linkedActivity(
    int32_t correlationId,
    const std::unordered_map<int64_t, int64_t>& correlationMap) {
  const auto& it = correlationMap.find(correlationId);
  if (it != correlationMap.end()) {
    const auto& it2 = activityMap_.find(it->second);
    if (it2 != activityMap_.end()) {
      return it2->second;
    }
  }
  return nullptr;
}

inline void CuptiActivityProfiler::handleGpuActivity(
    const ITraceActivity& act,
    ActivityLogger* logger) {
  if (outOfRange(act)) {
    return;
  }
  checkTimestampOrder(&act);
  VLOG(2) << act.correlationId() << ": " << act.name();
  recordStream(act.deviceId(), act.resourceId(), "");
  seenDeviceStreams_.insert({act.deviceId(), act.resourceId()});

  act.log(*logger);
  setGpuActivityPresent(true);
  updateGpuNetSpan(act);
  if (derivedConfig_->profileActivityTypes().contains(
          ActivityType::GPU_USER_ANNOTATION)) {
    const auto& it = userCorrelationMap_.find(act.correlationId());
    if (it != userCorrelationMap_.end()) {
      const auto& it2 = activityMap_.find(it->second);
      if (it2 != activityMap_.end()) {
        recordStream(act.deviceId(), act.resourceId(), "context");
        gpuUserEventMap_.insertOrExtendEvent(*it2->second, act);
      }
    }
  }
}

#ifdef HAS_CUPTI
template <class T>
inline void CuptiActivityProfiler::handleGpuActivity(
    const T* act,
    ActivityLogger* logger) {
  const ITraceActivity* linked =
      linkedActivity(act->correlationId, cpuCorrelationMap_);
  const auto& gpu_activity =
      traceBuffers_->addActivityWrapper(GpuActivity<T>(act, linked));
  handleGpuActivity(gpu_activity, logger);
}

template <class T>
static inline void updateCtxToDeviceId(const T* act) {
  if (!ctxToDeviceId().contains(act->contextId)) {
    ctxToDeviceId()[act->contextId] = act->deviceId;
  }
}

uint32_t contextIdtoDeviceId(uint32_t contextId) {
  auto it = ctxToDeviceId().find(contextId);
  return it != ctxToDeviceId().end() ? it->second : 0;
}

void CuptiActivityProfiler::handleCuptiActivity(
    const CUpti_Activity* record,
    ActivityLogger* logger) {
  switch (record->kind) {
    case CUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION:
      handleCorrelationActivity(
          reinterpret_cast<const CUpti_ActivityExternalCorrelation*>(record));
      break;
    case CUPTI_ACTIVITY_KIND_RUNTIME:
      handleRuntimeActivity(
          reinterpret_cast<const CUpti_ActivityAPI*>(record), logger);
      break;
    case CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL: {
      auto kernel = reinterpret_cast<const CUpti_ActivityKernel4*>(record);
      // Register all kernels launches so we could correlate them with other
      // events.
      KernelRegistry::singleton()->recordKernel(
          kernel->deviceId, demangle(kernel->name), kernel->correlationId);
      handleGpuActivity(kernel, logger);
      updateCtxToDeviceId(kernel);
      break;
    }
    case CUPTI_ACTIVITY_KIND_SYNCHRONIZATION:
      handleCudaSyncActivity(
          reinterpret_cast<const CUpti_ActivitySynchronization*>(record),
          logger);
      break;
    case CUPTI_ACTIVITY_KIND_CUDA_EVENT:
      handleCudaEventActivity(
          reinterpret_cast<const CUpti_ActivityCudaEvent*>(record));
      break;
    case CUPTI_ACTIVITY_KIND_MEMCPY:
      handleGpuActivity(
          reinterpret_cast<const CUpti_ActivityMemcpy*>(record), logger);
      break;
    case CUPTI_ACTIVITY_KIND_MEMCPY2:
      handleGpuActivity(
          reinterpret_cast<const CUpti_ActivityMemcpy2*>(record), logger);
      break;
    case CUPTI_ACTIVITY_KIND_MEMSET:
      handleGpuActivity(
          reinterpret_cast<const CUpti_ActivityMemset*>(record), logger);
      break;
    case CUPTI_ACTIVITY_KIND_OVERHEAD:
      handleOverheadActivity(
          reinterpret_cast<const CUpti_ActivityOverhead*>(record), logger);
      break;
    case CUPTI_ACTIVITY_KIND_DRIVER:
      handleDriverActivity(
          reinterpret_cast<const CUpti_ActivityAPI*>(record), logger);
      break;
    default:
      LOG(WARNING) << "Unexpected activity type: " << record->kind;
      ecs_.unexepected_cuda_events++;
      break;
  }
}
#endif // HAS_CUPTI

#ifdef HAS_ROCTRACER
template <class T>
void CuptiActivityProfiler::handleRuntimeActivity(
    const T* activity,
    ActivityLogger* logger) {
  int32_t tid = activity->tid;
  const auto& it = resourceInfo_.find({processId(), tid});
  if (it != resourceInfo_.end()) {
    tid = it->second.id;
  }
  const ITraceActivity* linked =
      linkedActivity(activity->id, cpuCorrelationMap_);
  const auto& runtime_activity =
      traceBuffers_->addActivityWrapper(RuntimeActivity<T>(activity, linked));
  checkTimestampOrder(&runtime_activity);
  if (outOfRange(runtime_activity)) {
    return;
  }
  runtime_activity.log(*logger);
  setGpuActivityPresent(true);
}

inline void CuptiActivityProfiler::handleGpuActivity(
    const roctracerAsyncRow* act,
    ActivityLogger* logger) {
  const ITraceActivity* linked = linkedActivity(act->id, cpuCorrelationMap_);
  const auto& gpu_activity =
      traceBuffers_->addActivityWrapper(GpuActivity(act, linked));
  handleGpuActivity(gpu_activity, logger);
}

void CuptiActivityProfiler::handleRoctracerActivity(
    const roctracerBase* record,
    ActivityLogger* logger) {
  switch (record->type) {
    case ROCTRACER_ACTIVITY_DEFAULT:
      handleRuntimeActivity(
          reinterpret_cast<const roctracerRow*>(record), logger);
      break;
    case ROCTRACER_ACTIVITY_KERNEL:
      handleRuntimeActivity(
          reinterpret_cast<const roctracerKernelRow*>(record), logger);
      break;
    case ROCTRACER_ACTIVITY_COPY:
      handleRuntimeActivity(
          reinterpret_cast<const roctracerCopyRow*>(record), logger);
      break;
    case ROCTRACER_ACTIVITY_MALLOC:
      handleRuntimeActivity(
          reinterpret_cast<const roctracerMallocRow*>(record), logger);
      break;
    case ROCTRACER_ACTIVITY_ASYNC:
      handleGpuActivity(
          reinterpret_cast<const roctracerAsyncRow*>(record), logger);
      break;
    case ROCTRACER_ACTIVITY_NONE:
    default:
      LOG(WARNING) << "Unexpected activity type: " << record->type;
      ecs_.unexepected_cuda_events++;
      break;
  }
}
#endif // HAS_ROCTRACER

const ITraceActivity* CuptiActivityProfiler::cpuActivity(
    int32_t correlationId) {
  const auto& it2 = activityMap_.find(correlationId);
  return (it2 != activityMap_.end()) ? it2->second : nullptr;
}

void CuptiActivityProfiler::configureChildProfilers() {
  // If child profilers are enabled create profiler sessions
  int64_t start_time_ms =
      duration_cast<milliseconds>(
          derivedConfig_->profileStartTime().time_since_epoch())
          .count();
  for (auto& profiler : profilers_) {
    LOG(INFO) << "[Profiler = " << profiler->name() << "] "
              << "Evaluating whether to run child profiler.";
    auto session = profiler->configure(
        start_time_ms,
        derivedConfig_->profileDuration().count(),
        derivedConfig_->profileActivityTypes(),
        *config_);
    if (session) {
      LOG(INFO) << "[Profiler = " << profiler->name() << "] "
                << "Running child profiler " << profiler->name() << " for "
                << derivedConfig_->profileDuration().count() << " ms";
      sessions_.push_back(std::move(session));
    } else {
      LOG(INFO) << "[Profiler = " << profiler->name() << "] "
                << "Not running child profiler.";
    }
  }
}

void CuptiActivityProfiler::configure(
    const Config& config,
    const time_point<system_clock>& now) {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  if (isActive()) {
    LOG(WARNING) << "CuptiActivityProfiler already busy, terminating";
    return;
  }
  ApproximateClockToUnixTimeConverter clockConverter;
  get_time_converter() = clockConverter.makeConverter();

  config_ = config.clone();

  // Ensure we're starting in a clean state
  resetTraceData();

#if !USE_GOOGLE_LOG
  // Add a LoggerObserverCollector to collect all logs during the trace.
  loggerCollectorMetadata_ = std::make_unique<LoggerCollector>();
  Logger::addLoggerObserver(loggerCollectorMetadata_.get());
#endif // !USE_GOOGLE_LOG

  derivedConfig_.reset();
  derivedConfig_ = std::make_unique<ConfigDerivedState>(*config_);

  // Check if now is a valid time to start.
  if (!derivedConfig_->canStart(now)) {
    return;
  }

  if (LOG_IS_ON(INFO)) {
    config_->printActivityProfilerConfig(LIBKINETO_DBG_STREAM);
  }
  if (!cpuOnly_ && !libkineto::api().client()) {
    gpuOnly_ = true;
    if (derivedConfig_->isProfilingByIteration()) {
      LOG(INFO) << "GPU-only tracing for " << config_->activitiesRunIterations()
                << " iterations";
    } else {
      LOG(INFO) << "GPU-only tracing for "
                << config_->activitiesDuration().count() << "ms";
    }
  }

  // Set useful metadata into the logger.
  LOGGER_OBSERVER_SET_TRACE_DURATION_MS(config_->activitiesDuration().count());
  LOGGER_OBSERVER_SET_TRACE_ID(config_->requestTraceID());
  LOGGER_OBSERVER_SET_GROUP_TRACE_ID(config_->requestGroupTraceID());
  if (!config_->requestTraceID().empty()) {
    addMetadata("trace_id", "\"" + config_->requestTraceID() + "\"");
  }

#if defined(HAS_CUPTI) || defined(HAS_ROCTRACER)
  if (!cpuOnly_) {
    // Enabling CUPTI activity tracing incurs a larger perf hit at first,
    // presumably because structures are allocated and initialized, callbacks
    // are activated etc. After a while the overhead decreases and stabilizes.
    // It's therefore useful to perform some warmup before starting recording.
    LOG(INFO) << "Enabling GPU tracing with max CUPTI buffer size "
              << config_->activitiesMaxGpuBufferSize() / 1024 / 1024 << "MB)";
    cupti_.setMaxBufferSize(config_->activitiesMaxGpuBufferSize());
    time_point<system_clock> timestamp;
    if (VLOG_IS_ON(1)) {
      timestamp = system_clock::now();
    }
    toggleState_.store(true);
#ifdef HAS_CUPTI
#ifdef _WIN32
    CUPTI_CALL(cuptiActivityRegisterTimestampCallback([]() -> uint64_t {
      auto system = std::chrono::time_point_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now());
      return system.time_since_epoch().count();
    }));
#else
#if CUDA_VERSION >= 11060
    use_cupti_tsc() = config_->getTSCTimestampFlag();
    if (use_cupti_tsc()) {
      CUPTI_CALL(cuptiActivityRegisterTimestampCallback(
          []() -> uint64_t { return getApproximateTime(); }));
    }
#endif // CUDA_VERSION >= 11060
#endif // _WIN32
    cupti_.enableCuptiActivities(
        config_->selectedActivityTypes(), config_->perThreadBufferEnabled());
#else // HAS_ROCTRACER
    cupti_.setMaxEvents(config_->maxEvents());
    cupti_.enableActivities(config_->selectedActivityTypes());
#endif
    if (VLOG_IS_ON(1)) {
      auto t2 = system_clock::now();
      addOverheadSample(
          setupOverhead_, duration_cast<microseconds>(t2 - timestamp).count());
    }
  }
#endif // HAS_CUPTI || HAS_ROCTRACER

  if (!profilers_.empty()) {
    configureChildProfilers();
  }
  rangeProfilingActive_ = config_->selectedActivityTypes().contains(
      ActivityType::CUDA_PROFILER_RANGE);

  if (libkineto::api().client()) {
    libkineto::api().client()->prepare(
        config_->isReportInputShapesEnabled(),
        config_->isProfileMemoryEnabled(),
        config_->isWithStackEnabled(),
        config_->isWithFlopsEnabled(),
        config_->isWithModulesEnabled());
  }

  if (derivedConfig_->isProfilingByIteration()) {
    LOG(INFO) << "Tracing starting on iteration = "
              << derivedConfig_->profileStartIteration();
    LOG(INFO) << "Tracing will end on iteration = "
              << derivedConfig_->profileEndIteration();
  } else {
    LOG(INFO) << "Tracing starting in "
              << duration_cast<seconds>(
                     derivedConfig_->profileStartTime() - now)
                     .count()
              << "s";
    LOG(INFO) << "Tracing will end in "
              << duration_cast<seconds>(derivedConfig_->profileEndTime() - now)
                     .count()
              << "s";
  }

  traceBuffers_ = std::make_unique<ActivityBuffers>();
  captureWindowStartTime_ = captureWindowEndTime_ = 0;
  currentRunloopState_ = RunloopState::Warmup;
}

bool CuptiActivityProfiler::getCollectTraceState() {
  std::lock_guard<std::recursive_mutex> guard(collectTraceStateMutex_);
  return isCollectingTrace;
}

void CuptiActivityProfiler::collectTrace(
    bool collection_done,
    const std::chrono::time_point<std::chrono::system_clock>& now) {
  if (libkineto::api().client()) {
    libkineto::api().client()->stop();
  }

#if defined(HAS_CUPTI) || defined(HAS_ROCTRACER)
  if (cupti_.stopCollection) {
    ecs_.cupti_stopped_early = cupti_.stopCollection;
    LOG(ERROR)
        << "State: CollectTrace stopped by CUPTI. (Buffer size configured is "
        << config_->activitiesMaxGpuBufferSize() / 1024 / 1024 << "MB)";
  }
#endif // HAS_CUPTI || HAS_ROCTRACER
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  stopTraceInternal(now);
  VLOG_IF(0, collection_done) << "Reached profile end time";
  UST_LOGGER_MARK_COMPLETED(kCollectionStage);
}

void CuptiActivityProfiler::ensureCollectTraceDone() {
  if (collectTraceThread_ && collectTraceThread_->joinable()) {
    collectTraceThread_->join();
    collectTraceThread_.reset(nullptr);
  }
}
void CuptiActivityProfiler::toggleCollectionDynamic(const bool enable) {
  if (toggleState_.load() == enable) {
    return;
  }
  toggleState_.store(enable);
#ifdef HAS_CUPTI
  CUDA_CALL(cudaDeviceSynchronize());
  if (enable) {
    cupti_.flushActivities();
    cupti_.enableCuptiActivities(
        derivedConfig_->profileActivityTypes(),
        derivedConfig_->isPerThreadBufferEnabled());
  } else {
    cupti_.flushActivities();
    cupti_.disableCuptiActivities(derivedConfig_->profileActivityTypes());
  }
#endif
#ifdef HAS_ROCTRACER
  CUDA_CALL(hipDeviceSynchronize());
  if (enable) {
    cupti_.flushActivities();
    cupti_.enableActivities(derivedConfig_->profileActivityTypes());
  } else {
    cupti_.flushActivities();
    cupti_.disableActivities(derivedConfig_->profileActivityTypes());
  }
#endif
#ifdef HAS_XPUPTI
  for (auto& session : sessions_) {
    auto xpu_session =
        dynamic_cast<XpuptiActivityProfilerSession*>(session.get());
    xpu_session->toggleCollectionDynamic(enable);
  }
#endif
}

void CuptiActivityProfiler::startTraceInternal(
    const time_point<system_clock>& now) {
  captureWindowStartTime_ = libkineto::timeSinceEpoch(now);
  VLOG(0) << "Warmup -> CollectTrace";
  for (auto& session : sessions_) {
    LOG(INFO) << "Starting child profiler session";
    session->start();
  }
  currentRunloopState_ = RunloopState::CollectTrace;
}

void CuptiActivityProfiler::stopTraceInternal(
    const time_point<system_clock>& now) {
  captureWindowEndTime_ = libkineto::timeSinceEpoch(now);
#if defined(HAS_CUPTI) || defined(HAS_ROCTRACER)
  if (!cpuOnly_) {
    time_point<system_clock> timestamp;
    if (VLOG_IS_ON(1)) {
      timestamp = system_clock::now();
    }
    toggleState_.store(false);
#ifdef HAS_CUPTI
    cupti_.disableCuptiActivities(derivedConfig_->profileActivityTypes());
#else
    cupti_.disableActivities(derivedConfig_->profileActivityTypes());
#endif
    if (VLOG_IS_ON(1)) {
      auto t2 = system_clock::now();
      addOverheadSample(
          setupOverhead_, duration_cast<microseconds>(t2 - timestamp).count());
    }
  }
#endif // HAS_CUPTI || HAS_ROCTRACER

  if (currentRunloopState_ == RunloopState::CollectTrace) {
    VLOG(0) << "CollectTrace -> ProcessTrace";
  } else {
    LOG(WARNING) << "Called stopTrace with state == "
                 << static_cast<std::underlying_type_t<RunloopState>>(
                        currentRunloopState_.load());
  }
  for (auto& session : sessions_) {
    LOG(INFO) << "Stopping child profiler session";
    session->stop();
  }
  currentRunloopState_ = RunloopState::ProcessTrace;
}

void CuptiActivityProfiler::resetInternal() {
  resetTraceData();
  currentRunloopState_ = RunloopState::WaitForRequest;
}

time_point<system_clock> CuptiActivityProfiler::performRunLoopStep(
    const time_point<system_clock>& now,
    const time_point<system_clock>& nextWakeupTime,
    int64_t currentIter) {
  auto new_wakeup_time = nextWakeupTime;
  bool warmup_done = false, collection_done = false;

  VLOG_IF(1, currentIter >= 0)
      << "Run loop on application step(), iteration = " << currentIter;

  switch (currentRunloopState_) {
    case RunloopState::CollectMemorySnapshot:
      LOG(WARNING)
          << "Entered CollectMemorySnapshot in Kineto Loop Step, skipping loop";
      break;
    case RunloopState::WaitForRequest:
      VLOG(1) << "State: WaitForRequest";
      // Nothing to do
      break;

    case RunloopState::Warmup:
      VLOG(1) << "State: Warmup";
      warmup_done = derivedConfig_->isWarmupDone(now, currentIter);
#if defined(HAS_CUPTI) || defined(HAS_ROCTRACER)
      // Flushing can take a while so avoid doing it close to the start time
      if (!cpuOnly_ && currentIter < 0 &&
          (derivedConfig_->isProfilingByIteration() ||
           nextWakeupTime < derivedConfig_->profileStartTime())) {
        cupti_.clearActivities();
      }

      if (cupti_.stopCollection) {
        // Go to process trace to clear any outstanding buffers etc
        std::lock_guard<std::recursive_mutex> guard(mutex_);
        stopTraceInternal(now);
        resetInternal();
        LOG(ERROR)
            << "State: Warmup stopped by CUPTI. (Buffer size configured is "
            << config_->activitiesMaxGpuBufferSize() / 1024 / 1024 << "MB)";
        UST_LOGGER_MARK_COMPLETED(kWarmUpStage);
        VLOG(0) << "Warmup -> WaitForRequest";
        break;
      }
#endif // HAS_CUPTI || HAS_ROCTRACER

      if (warmup_done) {
        UST_LOGGER_MARK_COMPLETED(kWarmUpStage);
        if (!derivedConfig_->isProfilingByIteration() &&
            (now > derivedConfig_->profileStartTime() + milliseconds(10))) {
          LOG(INFO) << "Tracing started "
                    << duration_cast<milliseconds>(
                           now - derivedConfig_->profileStartTime())
                           .count()
                    << "ms late!";
        } else {
          LOG(INFO) << "Tracing started";
        }
        startTrace(now);
        if (libkineto::api().client()) {
          libkineto::api().client()->start();
        }
        if (nextWakeupTime > derivedConfig_->profileEndTime()) {
          new_wakeup_time = derivedConfig_->profileEndTime();
        }
      } else if (nextWakeupTime > derivedConfig_->profileStartTime()) {
        new_wakeup_time = derivedConfig_->profileStartTime();
      }

      break;

    case RunloopState::CollectTrace:
      VLOG(1) << "State: CollectTrace";
      collection_done = derivedConfig_->isCollectionDone(now, currentIter);

      if (collection_done
#if defined(HAS_CUPTI) || defined(HAS_ROCTRACER)
          || cupti_.stopCollection
#endif // HAS_CUPTI || HAS_ROCTRACER
      ) {
        // Update runloop state first to prevent further updates to shared
        // state
        LOG(INFO) << "Tracing complete.";
        VLOG_IF(1, currentIter >= 0)
            << "This state change was invoked by application's step() call";

        // currentIter >= 0 means this is called from the step() api of
        // the profile in pytorch main thread, it should be executed in
        // another thread in case pytorch main thread is blocked
        if (currentIter >= 0) {
          // if collectTraceThread_ is already running, there's no need to
          // execute collectTrace twice.
          // Do not call collectTrace when profilerThread_ is collecting
          // Trace. Otherwise, libkineto::api().client()->stop will be called
          // twice, which leads to an unrecoverable ::c10:Error at
          // disableProfiler
          if (!collectTraceThread_ && !getCollectTraceState()) {
            std::lock_guard<std::recursive_mutex> guard(mutex_);
            collectTraceThread_ = std::make_unique<std::thread>(
                &CuptiActivityProfiler::collectTrace,
                this,
                collection_done,
                now);
          }
          break;
        }
        // this is executed in profilerThread_
        {
          std::lock_guard<std::recursive_mutex> guard(collectTraceStateMutex_);
          isCollectingTrace = true;
        }
        collectTrace(collection_done, now);
        {
          std::lock_guard<std::recursive_mutex> guard(collectTraceStateMutex_);
          isCollectingTrace = false;
        }
      } else if (derivedConfig_->isProfilingByIteration()) {
        // nothing to do here
      } else if (
          now < derivedConfig_->profileEndTime() &&
          derivedConfig_->profileEndTime() < nextWakeupTime) {
        new_wakeup_time = derivedConfig_->profileEndTime();
      }

      break;

    case RunloopState::ProcessTrace:
      VLOG(1) << "State: ProcessTrace";
      // skip this state transition if it called from the step() api
      // of the profiler.
      // else it could lead to a race between the profiler thread and an
      // application thread calling step()
      if (currentIter >= 0) {
        return new_wakeup_time;
      }

      // Before processing, we should wait for collectTrace thread to be done.
      ensureCollectTraceDone();

      // FIXME: Probably want to allow interruption here
      // for quickly handling trace request via synchronous API
      std::lock_guard<std::recursive_mutex> guard(mutex_);
      processTraceInternal(*logger_);
      UST_LOGGER_MARK_COMPLETED(kPostProcessingStage);
      resetInternal();
      VLOG(0) << "ProcessTrace -> WaitForRequest";
      break;
  }

  return new_wakeup_time;
}

void CuptiActivityProfiler::performMemoryLoop(
    const string& path,
    uint32_t profile_time,
    ActivityLogger* logger,
    Config& config) {
  currentRunloopState_ = RunloopState::CollectMemorySnapshot;
  if (libkineto::api().client()) {
    libkineto::api().client()->start_memory_profile();
    LOG(INFO) << "Running memory profiling for " << profile_time << " ms";
    std::this_thread::sleep_for(std::chrono::milliseconds(profile_time));
    LOG(INFO) << "Exporting memory profiling results to " << path;
    libkineto::api().client()->export_memory_profile(path);
    libkineto::api().client()->stop_memory_profile();
    LOG(INFO) << "Finalizing trace";
    logger->finalizeMemoryTrace(path, config);
  }
  currentRunloopState_ = RunloopState::WaitForRequest;
}

void CuptiActivityProfiler::finalizeTrace(
    const Config& config,
    ActivityLogger& logger) {
  LOG(INFO) << "CPU Traces Recorded:";
  {
    for (const auto& it : iterationCountMap_) {
      LOG(INFO) << it.first << ": " << it.second << " span(s) recorded";
    }
    iterationCountMap_.clear();
  }

  // Thread & stream info
  for (const auto& pair : resourceInfo_) {
    const auto& resource = pair.second;
    logger.handleResourceInfo(resource, captureWindowStartTime_);
  }

  bool use_default_device_info = true;
  for (auto& session : sessions_) {
    auto device_info = session->getDeviceInfo();
    if (device_info != nullptr) {
      use_default_device_info = false;
      logger.handleDeviceInfo(*device_info, captureWindowStartTime_);
    }

    auto resource_infos = session->getResourceInfos();
    for (const auto& resource_info : resource_infos) {
      logger.handleResourceInfo(resource_info, captureWindowStartTime_);
    }
  }

  // Process names
  int32_t pid = processId();
  string process_name = processName(pid);
  if (!process_name.empty()) {
    logger.handleDeviceInfo(
        {pid, pid, process_name, "CPU"}, captureWindowStartTime_);
    if (!cpuOnly_ && use_default_device_info) {
      // Usually, GPU events use device id as pid (0-7).
      // In some cases, CPU sockets are numbered starting from 0.
      // In the worst case, 8 CPU sockets + 8 GPUs, so the max GPU ID is 15.
      constexpr int kMaxGpuID = 15;
      // sortIndex is gpu + kExceedMaxPid to put GPU tracks at the bottom
      // of the trace timelines.
      for (int gpu = 0; gpu <= kMaxGpuID; gpu++) {
        logger.handleDeviceInfo(
            {gpu,
             gpu + kExceedMaxPid,
             process_name,
             fmt::format("GPU {}", gpu)},
            captureWindowStartTime_);
      }
    }
  }

  for (const auto& iterations : traceSpans_) {
    for (const auto& span_pair : iterations.second) {
      const TraceSpan& gpu_span = span_pair.second;
      if (gpu_span.opCount > 0) {
        logger.handleTraceSpan(gpu_span);
      }
    }
  }

#ifdef HAS_CUPTI
  // Overhead info
  overheadInfo_.emplace_back("CUPTI Overhead");
  for (const auto& info : overheadInfo_) {
    logger.handleOverheadInfo(info, captureWindowStartTime_);
  }
#endif // HAS_CUPTI

  gpuUserEventMap_.logEvents(&logger);

  for (auto& session : sessions_) {
    auto trace_buffer = session->getTraceBuffer();
    if (trace_buffer) {
      // Set child start time to profiling start time if not set
      if (trace_buffer->span.startTime == 0) {
        trace_buffer->span.startTime = captureWindowStartTime_;
      }
      traceBuffers_->cpu.push_back(std::move(trace_buffer));
    }
  }

  // Logger Metadata contains a map of LOGs collected in Kineto
  //   logger_level -> List of log lines
  // This will be added into the trace as metadata.
  std::unordered_map<std::string, std::vector<std::string>> loggerMD =
      getLoggerMetadata();
  logger.finalizeTrace(
      config, std::move(traceBuffers_), captureWindowEndTime_, loggerMD);
}

std::unordered_map<std::string, std::vector<std::string>>
CuptiActivityProfiler::getLoggerMetadata() {
  std::unordered_map<std::string, std::vector<std::string>> loggerMD;

#if !USE_GOOGLE_LOG
  // Save logs from LoggerCollector objects into Trace metadata.
  auto LoggerMDMap = loggerCollectorMetadata_->extractCollectorMetadata();
  for (auto& md : LoggerMDMap) {
    loggerMD[toString(md.first)] = md.second;
  }
#endif // !USE_GOOGLE_LOG
  return loggerMD;
}

void CuptiActivityProfiler::pushCorrelationId(uint64_t id) {
#ifdef HAS_CUPTI
  CuptiActivityApi::pushCorrelationID(
      id, CuptiActivityApi::CorrelationFlowType::Default);
#endif // HAS_CUPTI
#ifdef HAS_ROCTRACER
  RoctracerActivityApi::pushCorrelationID(
      id, RoctracerActivityApi::CorrelationFlowType::Default);
#endif
  for (auto& session : sessions_) {
    session->pushCorrelationId(id);
  }
}

void CuptiActivityProfiler::popCorrelationId() {
#ifdef HAS_CUPTI
  CuptiActivityApi::popCorrelationID(
      CuptiActivityApi::CorrelationFlowType::Default);
#endif // HAS_CUPTI
#ifdef HAS_ROCTRACER
  RoctracerActivityApi::popCorrelationID(
      RoctracerActivityApi::CorrelationFlowType::Default);
#endif
  for (auto& session : sessions_) {
    session->popCorrelationId();
  }
}

void CuptiActivityProfiler::pushUserCorrelationId(uint64_t id) {
#ifdef HAS_CUPTI
  CuptiActivityApi::pushCorrelationID(
      id, CuptiActivityApi::CorrelationFlowType::User);
#endif // HAS_CUPTI
#ifdef HAS_ROCTRACER
  RoctracerActivityApi::pushCorrelationID(
      id, RoctracerActivityApi::CorrelationFlowType::User);
#endif
  for (auto& session : sessions_) {
    session->pushUserCorrelationId(id);
  }
}

void CuptiActivityProfiler::popUserCorrelationId() {
#ifdef HAS_CUPTI
  CuptiActivityApi::popCorrelationID(
      CuptiActivityApi::CorrelationFlowType::User);
#endif // HAS_CUPTI
#ifdef HAS_ROCTRACER
  RoctracerActivityApi::popCorrelationID(
      RoctracerActivityApi::CorrelationFlowType::User);
#endif
  for (auto& session : sessions_) {
    session->popUserCorrelationId();
  }
}

void CuptiActivityProfiler::resetTraceData() {
#if defined(HAS_CUPTI) || defined(HAS_ROCTRACER)
  if (!cpuOnly_) {
    cupti_.clearActivities();
    cupti_.teardownContext();
#ifdef HAS_CUPTI
    KernelRegistry::singleton()->clear();
#endif
  }
#endif // HAS_CUPTI || HAS_ROCTRACER
  activityMap_.clear();
  cpuCorrelationMap_.clear();
  correlatedCudaActivities_.clear();
  gpuUserEventMap_.clear();
  traceSpans_.clear();
  clientActivityTraceMap_.clear();
  seenDeviceStreams_.clear();
  logQueue_.clear();
  traceBuffers_ = nullptr;
  metadata_.clear();
  sessions_.clear();
  resourceOverheadCount_ = 0;
  ecs_ = ErrorCounts{};
#if !USE_GOOGLE_LOG
  Logger::removeLoggerObserver(loggerCollectorMetadata_.get());
#endif // !USE_GOOGLE_LOG
}

} // namespace KINETO_NAMESPACE
