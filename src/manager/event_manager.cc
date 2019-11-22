/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common/debug.h"
#include "common/expected.h"
#include "common/rx_async.h"
#include "db/app_component_name.h"
#include "db/file_models.h"
#include "db/models.h"
#include "manager/event_manager.h"
#include "perfetto/rx_producer.h"
#include "prefetcher/read_ahead.h"
#include "prefetcher/task_id.h"

#include <android-base/properties.h>
#include <rxcpp/rx.hpp>

#include <atomic>
#include <functional>
#include <utils/Trace.h>

using rxcpp::observe_on_one_worker;

namespace iorap::manager {

using binder::AppLaunchEvent;
using binder::JobScheduledEvent;
using binder::RequestId;
using binder::TaskResult;

using common::AsyncPool;
using common::RxAsync;

using perfetto::PerfettoStreamCommand;
using perfetto::PerfettoTraceProto;

using db::AppComponentName;

// Main logic of the #OnAppLaunchEvent scan method.
//
// All functions are called from the same thread as the event manager
// functions.
//
// This is a data type, it's moved (std::move) around from one iteration
// of #scan to another.
struct AppLaunchEventState {
  std::optional<AppComponentName> component_name_;
  // Sequence ID is shared amongst the same app launch sequence,
  // but changes whenever a new app launch sequence begins.
  size_t sequence_id_ = static_cast<size_t>(-1);
  std::optional<AppLaunchEvent::Temperature> temperature_;

  // Push data to perfetto rx chain for associating
  // the raw_trace with the history_id.
  std::optional<rxcpp::subscriber<int>> history_id_subscriber_;
  rxcpp::observable<int> history_id_observable_;

  std::optional<uint64_t> intent_started_ns_;
  std::optional<uint64_t> total_time_ns_;

  // Used by kReportFullyDrawn to find the right history_id.
  // We assume no interleaving between different sequences.
  // This assumption is checked in the Java service code.
  std::optional<uint64_t> recent_history_id_;

  // labeled as 'shared' due to rx not being able to handle move-only objects.
  // lifetime: in practice equivalent to unique_ptr.
  std::shared_ptr<prefetcher::ReadAhead> read_ahead_;
  bool allowed_readahead_{true};
  bool is_read_ahead_{false};
  std::optional<prefetcher::TaskId> read_ahead_task_;

  bool allowed_tracing_{true};
  bool is_tracing_{false};
  std::optional<rxcpp::composite_subscription> rx_lifetime_;
  std::vector<rxcpp::composite_subscription> rx_in_flight_;

  borrowed<perfetto::RxProducerFactory*> perfetto_factory_;  // not null
  borrowed<observe_on_one_worker*> thread_;  // not null
  borrowed<observe_on_one_worker*> io_thread_;  // not null
  borrowed<AsyncPool*> async_pool_;  // not null

  explicit AppLaunchEventState(borrowed<perfetto::RxProducerFactory*> perfetto_factory,
                               bool allowed_readahead,
                               bool allowed_tracing,
                               borrowed<observe_on_one_worker*> thread,
                               borrowed<observe_on_one_worker*> io_thread,
                               borrowed<AsyncPool*> async_pool)
    : read_ahead_{std::make_shared<prefetcher::ReadAhead>()}
  {
    perfetto_factory_ = perfetto_factory;
    DCHECK(perfetto_factory_ != nullptr);

    allowed_readahead_ = allowed_readahead;
    allowed_tracing_ = allowed_tracing;

    thread_ = thread;
    DCHECK(thread_ != nullptr);

    io_thread_ = io_thread;
    DCHECK(io_thread_ != nullptr);

    async_pool_ = async_pool;
    DCHECK(async_pool_ != nullptr);
  }

  // Updates the values in this struct only as a side effect.
  //
  // May create and fire a new rx chain on the same threads as passed
  // in by the constructors.
  void OnNewEvent(const AppLaunchEvent& event) {
    LOG(VERBOSE) << "AppLaunchEventState#OnNewEvent: " << event;

    android::ScopedTrace trace_db_init{ATRACE_TAG_PACKAGE_MANAGER,
                                       "IorapNativeService::OnAppLaunchEvent"};

    using Type = AppLaunchEvent::Type;

    DCHECK_GE(event.sequence_id, 0);
    sequence_id_ = static_cast<size_t>(event.sequence_id);

    switch (event.type) {
      case Type::kIntentStarted: {
        // Create a new history ID chain for each new app start-up sequence.
        auto history_id_observable = rxcpp::observable<>::create<int>(
          [&](rxcpp::subscriber<int> subscriber) {
            history_id_subscriber_ = std::move(subscriber);
            LOG(VERBOSE) << " set up the history id subscriber ";
          })
          .tap([](int history_id) { LOG(VERBOSE) << " tap rx history id = " << history_id; })
          .replay(1);  // Remember the history id in case we subscribe too late.

        history_id_observable_ = history_id_observable;

        // Immediately turn observable hot, creating the subscriber.
        history_id_observable.connect();

        DCHECK(!IsTracing());
        // Optimistically start tracing if we have the activity in the intent.
        if (!event.intent_proto->has_component()) {
          // Can't do anything if there is no component in the proto.
          LOG(VERBOSE) << "AppLaunchEventState#OnNewEvent: no component, can't trace";
          break;
        }

        const std::string& package_name = event.intent_proto->component().package_name();
        const std::string& class_name = event.intent_proto->component().class_name();
        AppComponentName component_name{package_name, class_name};

        component_name_ = component_name;

        if (allowed_readahead_) {
          StartReadAhead(sequence_id_, component_name);
        }
        if (allowed_tracing_) {
          rx_lifetime_ = StartTracing(std::move(component_name));
        }

        if (event.timestamp_nanos >= 0) {
          intent_started_ns_ = event.timestamp_nanos;
        } else {
          LOG(WARNING) << "Negative event timestamp: " << event.timestamp_nanos;
        }

        break;
      }
      case Type::kIntentFailed:
        AbortTrace();
        AbortReadAhead();

        if (history_id_subscriber_) {
          history_id_subscriber_->on_error(rxcpp::util::make_error_ptr(
            std::ios_base::failure("Aborting due to intent failed")));
          history_id_subscriber_ = std::nullopt;
        }

        break;
      case Type::kActivityLaunched: {
        // Cancel tracing for warm/hot.
        // Restart tracing if the activity was unexpected.

        AppLaunchEvent::Temperature temperature = event.temperature;
        temperature_ = temperature;
        if (temperature != AppLaunchEvent::Temperature::kCold) {
          LOG(DEBUG) << "AppLaunchEventState#OnNewEvent aborting trace due to non-cold temperature";

          AbortTrace();
          AbortReadAhead();
        } else if (!IsTracing() || !IsReadAhead()) {  // and the temperature is Cold.
          // Start late trace when intent didn't have a component name
          LOG(VERBOSE) << "AppLaunchEventState#OnNewEvent need to start new trace";

          const std::string& title = event.activity_record_proto->identifier().title();
          if (!AppComponentName::HasAppComponentName(title)) {
            // Proto comment claim this is sometimes a window title.
            // We need the actual 'package/component' here, so just ignore it if it's a title.
            LOG(WARNING) << "App launched without a component name: " << event;
            break;
          }

          AppComponentName component_name = AppComponentName::FromString(title);

          component_name_ = component_name;

          if (allowed_readahead_ && !IsReadAhead()) {
            StartReadAhead(sequence_id_, component_name);
          }
          if (allowed_tracing_ && !IsTracing()) {
            rx_lifetime_ = StartTracing(std::move(component_name));
          }
        } else {
          // FIXME: match actual component name against intent component name.
          // abort traces if they don't match.

          if (allowed_tracing_) {
            LOG(VERBOSE) << "AppLaunchEventState#OnNewEvent already tracing";
          }
          LOG(VERBOSE) << "AppLaunchEventState#OnNewEvent already doing readahead";
        }
        break;
      }
      case Type::kActivityLaunchFinished:
        if (event.timestamp_nanos >= 0) {
           total_time_ns_ = event.timestamp_nanos;
        }
        RecordDbLaunchHistory();
        // Finish tracing and collect trace buffer.
        //
        // TODO: this happens automatically when perfetto finishes its
        // trace duration.
        if (IsTracing()) {
          MarkPendingTrace();
        }
        FinishReadAhead();
        break;
      case Type::kActivityLaunchCancelled:
        // Abort tracing.
        AbortTrace();
        AbortReadAhead();
        break;
      case Type::kReportFullyDrawn: {
        if (!recent_history_id_) {
          LOG(WARNING) << "Dangling kReportFullyDrawn event";
          return;
        }
        UpdateReportFullyDrawn(event.timestamp_nanos, *recent_history_id_);
        break;
      }
      default:
        DCHECK(false) << "invalid type: " << event;  // binder layer should've rejected this.
        LOG(ERROR) << "invalid type: " << event;  // binder layer should've rejected this.
    }
  }

  // Is there an in-flight readahead task currently?
  bool IsReadAhead() const {
    return read_ahead_task_.has_value();
  }

  void StartReadAhead(size_t id, const AppComponentName& component_name) {
    DCHECK(allowed_readahead_);
    DCHECK(!IsReadAhead());

    // This is changed from "/data/misc/iorapd/" for testing purpose.
    // TODO: b/139831359.
    std::string file_path = "/product/iorap-trace/";
    file_path += component_name.ToMakeFileSafeEncodedPkgString();
    file_path += ".compiled_trace.pb";

    prefetcher::TaskId task{id, std::move(file_path)};
    read_ahead_->BeginTask(task);
    // TODO: non-void return signature?

    read_ahead_task_ = std::move(task);
  }

  void FinishReadAhead() {
    // if no readahead task exist, do nothing.
    if (!IsReadAhead()){
      return;
    }

    read_ahead_->FinishTask(*read_ahead_task_);
    read_ahead_task_ = std::nullopt;
  }

  void AbortReadAhead() {
    FinishReadAhead();
  }

  bool IsTracing() const {
    return is_tracing_;
  }

  rxcpp::composite_subscription StartTracing(AppComponentName component_name) {
    DCHECK(allowed_tracing_);
    DCHECK(!IsTracing());

    auto /*observable<PerfettoStreamCommand>*/ perfetto_commands =
      rxcpp::observable<>::just(PerfettoStreamCommand::kStartTracing)
          // wait 1x
          .concat(
              // Pick a value longer than the perfetto config delay_ms, so that we send
              // 'kShutdown' after tracing has already finished.
              rxcpp::observable<>::interval(std::chrono::milliseconds(10000))
                  .take(2)  // kStopTracing, kShutdown.
                  .map([](int value) {
                         // value is 1,2,3,...
                         return static_cast<PerfettoStreamCommand>(value);  // 1,2, ...
                       })
          );

    auto /*observable<PerfettoTraceProto>*/ trace_proto_stream =
        perfetto_factory_->CreateTraceStream(perfetto_commands);
    // This immediately connects to perfetto asynchronously.
    //
    // TODO: create a perfetto handle earlier, to minimize perfetto startup latency.

    rxcpp::composite_subscription lifetime;

    auto stream_via_threads = trace_proto_stream
      .tap([](const PerfettoTraceProto& trace_proto) {
             LOG(VERBOSE) << "StartTracing -- PerfettoTraceProto received (1)";
           })
      .combine_latest(history_id_observable_)
      .observe_on(*thread_)   // All work prior to 'observe_on' is handled on thread_.
      .subscribe_on(*thread_)   // All work prior to 'observe_on' is handled on thread_.
      .observe_on(*io_thread_)  // Write data on an idle-class-priority thread.
      .tap([](std::tuple<PerfettoTraceProto, int> trace_tuple) {
             LOG(VERBOSE) << "StartTracing -- PerfettoTraceProto received (2)";
           });

    db::VersionedComponentName versioned_component_name{component_name.package,
                                                        component_name.activity_name,
                                                        /*version*/std::nullopt};  // TODO: version
    lifetime = RxAsync::SubscribeAsync(*async_pool_,
        std::move(stream_via_threads),
        /*on_next*/[versioned_component_name]
        (std::tuple<PerfettoTraceProto, int> trace_tuple) {
          PerfettoTraceProto& trace_proto = std::get<0>(trace_tuple);
          int history_id = std::get<1>(trace_tuple);

          db::PerfettoTraceFileModel file_model =
            db::PerfettoTraceFileModel::CalculateNewestFilePath(versioned_component_name);

          std::string file_path = file_model.FilePath();

          if (!file_model.MkdirWithParents()) {
            LOG(ERROR) << "Cannot save TraceBuffer; failed to mkdirs " << file_path;
            return;
          }

          if (!trace_proto.WriteFullyToFile(file_path)) {
            LOG(ERROR) << "Failed to save TraceBuffer to " << file_path;
          } else {
            LOG(INFO) << "Perfetto TraceBuffer saved to file: " << file_path;

            db::DbHandle db{db::SchemaModel::GetSingleton()};
            std::optional<db::RawTraceModel> raw_trace =
                db::RawTraceModel::Insert(db, history_id, file_path);

            if (!raw_trace) {
              LOG(ERROR) << "Failed to insert raw_traces for " << file_path;
            } else {
              LOG(VERBOSE) << "Inserted into db: " << *raw_trace;

              // Ensure we don't have too many files per-app.
              db::PerfettoTraceFileModel::DeleteOlderFiles(db, versioned_component_name);
            }
          }
        },
        /*on_error*/[](rxcpp::util::error_ptr err) {
          LOG(ERROR) << "Perfetto trace proto collection error: " << rxcpp::util::what(err);
        });

    is_tracing_ = true;

    return lifetime;
  }

  void AbortTrace() {
    LOG(VERBOSE) << "AppLaunchEventState - AbortTrace";
    DCHECK(allowed_tracing_);

    // if the tracing is not running, do nothing.
    if (!IsTracing()){
      return;
    }

    is_tracing_ = false;
    if (rx_lifetime_) {
      // TODO: it would be good to call perfetto Destroy.

      std::remove(rx_in_flight_.begin(),
                  rx_in_flight_.end(),
                  *rx_lifetime_);

      LOG(VERBOSE) << "AppLaunchEventState - AbortTrace - Unsubscribe";
      rx_lifetime_->unsubscribe();

      rx_lifetime_.reset();
    }
  }

  void MarkPendingTrace() {
    LOG(VERBOSE) << "AppLaunchEventState - MarkPendingTrace";
    DCHECK(allowed_tracing_);
    DCHECK(is_tracing_);
    DCHECK(rx_lifetime_.has_value());

    if (rx_lifetime_) {
      LOG(VERBOSE) << "AppLaunchEventState - MarkPendingTrace - lifetime moved";
      // Don't unsubscribe because that would cause the perfetto TraceBuffer
      // to get dropped on the floor.
      //
      // Instead, we want to let it finish and write it out to a file.
      rx_in_flight_.push_back(*std::move(rx_lifetime_));
      rx_lifetime_.reset();
    } else {
      LOG(VERBOSE) << "AppLaunchEventState - MarkPendingTrace - lifetime was empty";
    }

    // FIXME: how do we clear this vector?
  }

  void RecordDbLaunchHistory() {
    std::optional<db::AppLaunchHistoryModel> history = InsertDbLaunchHistory();

    // RecordDbLaunchHistory happens-after kIntentStarted
    CHECK(history_id_subscriber_.has_value()) << "Logic error? "
                                              << "Should always have a subscriber here.";

    // Ensure that the history id rx chain is terminated either with an error or with
    // the newly inserted app_launch_histories.id
    if (!history) {
      history_id_subscriber_->on_error(rxcpp::util::make_error_ptr(
          std::ios_base::failure("Failed to insert history id")));
      recent_history_id_ = std::nullopt;
    } else {
      // Note: we must have already subscribed, or this value will disappear.
      LOG(VERBOSE) << "history_id_subscriber on_next history_id=" << history->id;
      history_id_subscriber_->on_next(history->id);
      history_id_subscriber_->on_completed();

      recent_history_id_ = history->id;
    }
    history_id_subscriber_ = std::nullopt;
  }

  std::optional<db::AppLaunchHistoryModel> InsertDbLaunchHistory() {
    // TODO: deferred queue into a different lower priority thread.
    if (!component_name_ || !temperature_) {
      LOG(VERBOSE) << "Skip RecordDbLaunchHistory, no component name available.";

      return std::nullopt;
    }

    android::ScopedTrace trace{ATRACE_TAG_PACKAGE_MANAGER,
                               "IorapNativeService::RecordDbLaunchHistory"};
    db::DbHandle db{db::SchemaModel::GetSingleton()};

    using namespace iorap::db;

    std::optional<ActivityModel> activity =
        ActivityModel::SelectOrInsert(db,
                                      component_name_->package,
                                      /*version*/std::nullopt,
                                      component_name_->activity_name);

    if (!activity) {
      LOG(WARNING) << "Failed to query activity row for : " << *component_name_;
      return std::nullopt;
    }

    auto temp = static_cast<db::AppLaunchHistoryModel::Temperature>(*temperature_);

    std::optional<AppLaunchHistoryModel> alh =
        AppLaunchHistoryModel::Insert(db,
                                      activity->id,
                                      temp,
                                      IsTracing(),
                                      IsReadAhead(),
                                      intent_started_ns_,
                                      total_time_ns_,
                                      // ReportFullyDrawn event normally occurs after this. Need update later.
                                      /* report_fully_drawn_ns= */ std::nullopt);
    //Repo
    if (!alh) {
      LOG(WARNING) << "Failed to insert app_launch_histories row";
      return std::nullopt;
    }

    LOG(VERBOSE) << "RecordDbLaunchHistory: " << *alh;
    return alh;
  }

  void UpdateReportFullyDrawn(int history_id, uint64_t timestamp_ns) {
    android::ScopedTrace trace{ATRACE_TAG_PACKAGE_MANAGER,
                               "IorapNativeService::UpdateReportFullyDrawn"};
    db::DbHandle db{db::SchemaModel::GetSingleton()};

    bool result =
        db::AppLaunchHistoryModel::UpdateReportFullyDrawn(db,
                                                          history_id,
                                                          timestamp_ns);

    if (!result) {
      LOG(WARNING) << "Failed to update app_launch_histories row";
    }
  }
};

// Convert callback pattern into reactive pattern.
struct AppLaunchEventSubject {
  using RefWrapper =
    std::reference_wrapper<const AppLaunchEvent>;

  AppLaunchEventSubject() {}

  void Subscribe(rxcpp::subscriber<RefWrapper> subscriber) {
    DCHECK(ready_ != true) << "Cannot Subscribe twice";

    subscriber_ = std::move(subscriber);

    // Release edge of synchronizes-with AcquireIsReady.
    ready_.store(true);
  }

  void OnNext(const AppLaunchEvent& e) {
    if (!AcquireIsReady()) {
      return;
    }

    if (!subscriber_->is_subscribed()) {
      return;
    }

    /*
     * TODO: fix upstream.
     *
     * Rx workaround: this fails to compile when
     * the observable is a reference type:
     *
     * external/Reactive-Extensions/RxCpp/Rx/v2/src/rxcpp/rx-observer.hpp:354:18: error: multiple overloads of 'on_next' instantiate to the same signature 'void (const iorap::binder::AppLaunchEvent &) const'
     *   virtual void on_next(T&&) const {};
     *
     * external/Reactive-Extensions/RxCpp/Rx/v2/src/rxcpp/rx-observer.hpp:353:18: note: previous declaration is here
     *   virtual void on_next(T&) const {};
     *
     * (The workaround is to use reference_wrapper instead
     *  of const AppLaunchEvent&)
     */
    subscriber_->on_next(std::cref(e));

  }

  void OnCompleted() {
    if (!AcquireIsReady()) {
      return;
    }

    subscriber_->on_completed();
  }

 private:
  bool AcquireIsReady() {
    // Synchronizes-with the release-edge in Subscribe.
    // This can happen much later, only once the subscription actually happens.

    // However, as far as I know, 'rxcpp::subscriber' is not thread safe,
    // (but the observable chain itself can be made thread-safe via #observe_on, etc).
    // so we must avoid reading it until it has been fully synchronized.
    //
    // TODO: investigate rxcpp subscribers and see if we can get rid of this atomics,
    // to make it simpler.
    return ready_.load();
  }

  // TODO: also track the RequestId ?

  std::atomic<bool> ready_{false};


  std::optional<rxcpp::subscriber<RefWrapper>> subscriber_;
};

// Convert callback pattern into reactive pattern.
struct JobScheduledEventSubject {
  JobScheduledEventSubject() {}

  void Subscribe(rxcpp::subscriber<std::pair<RequestId, JobScheduledEvent>> subscriber) {
    DCHECK(ready_ != true) << "Cannot Subscribe twice";

    subscriber_ = std::move(subscriber);

    // Release edge of synchronizes-with AcquireIsReady.
    ready_.store(true);
  }

  void OnNext(RequestId request_id, JobScheduledEvent e) {
    if (!AcquireIsReady()) {
      return;
    }

    if (!subscriber_->is_subscribed()) {
      return;
    }

    subscriber_->on_next(std::pair<RequestId, JobScheduledEvent>{std::move(request_id), std::move(e)});

  }

  void OnCompleted() {
    if (!AcquireIsReady()) {
      return;
    }

    subscriber_->on_completed();
  }

 private:
  bool AcquireIsReady() {
    // Synchronizes-with the release-edge in Subscribe.
    // This can happen much later, only once the subscription actually happens.

    // However, as far as I know, 'rxcpp::subscriber' is not thread safe,
    // (but the observable chain itself can be made thread-safe via #observe_on, etc).
    // so we must avoid reading it until it has been fully synchronized.
    //
    // TODO: investigate rxcpp subscribers and see if we can get rid of this atomics,
    // to make it simpler.
    return ready_.load();
  }

  // TODO: also track the RequestId ?

  std::atomic<bool> ready_{false};

  std::optional<rxcpp::subscriber<std::pair<RequestId, JobScheduledEvent>>> subscriber_;
};

class EventManager::Impl {
 public:
  Impl(/*borrow*/perfetto::RxProducerFactory& perfetto_factory)
    : perfetto_factory_(perfetto_factory),
      worker_thread_(rxcpp::observe_on_new_thread()),
      worker_thread2_(rxcpp::observe_on_new_thread()),
      io_thread_(perfetto::ObserveOnNewIoThread()) {

    // TODO: read all properties from one config class.
    tracing_allowed_ = ::android::base::GetBoolProperty("iorapd.perfetto.enable", /*default*/false);
    readahead_allowed_ = ::android::base::GetBoolProperty("iorapd.readahead.enable",
                                                          /*default*/false);

    rx_lifetime_ = InitializeRxGraph();
    rx_lifetime_jobs_ = InitializeRxGraphForJobScheduledEvents();
  }

  void SetTaskResultCallbacks(std::shared_ptr<TaskResultCallbacks> callbacks) {
    DCHECK(callbacks_.expired());
    callbacks_ = callbacks;
  }

  void Join() {
    async_pool_.Join();
  }

  bool OnAppLaunchEvent(RequestId request_id,
                        const AppLaunchEvent& event) {
    LOG(VERBOSE) << "EventManager::OnAppLaunchEvent("
                 << "request_id=" << request_id.request_id << ","
                 << event;

    app_launch_event_subject_.OnNext(event);

    return true;
  }

  bool OnJobScheduledEvent(RequestId request_id,
                           const JobScheduledEvent& event) {
    LOG(VERBOSE) << "EventManager::OnJobScheduledEvent("
                 << "request_id=" << request_id.request_id << ",event=TODO).";

    job_scheduled_event_subject_.OnNext(std::move(request_id), event);

    return true;  // No errors.
  }

  rxcpp::composite_subscription InitializeRxGraph() {
    LOG(VERBOSE) << "EventManager::InitializeRxGraph";

    app_launch_events_ = rxcpp::observable<>::create<AppLaunchEventRefWrapper>(
      [&](rxcpp::subscriber<AppLaunchEventRefWrapper> subscriber) {
        app_launch_event_subject_.Subscribe(std::move(subscriber));
      });

    rxcpp::composite_subscription lifetime;

    if (!tracing_allowed_) {
      LOG(WARNING) << "Tracing disabled by iorapd.perfetto.enable=false";
    }
    if (!readahead_allowed_) {
      LOG(WARNING) << "Readahead disabled by iorapd.readahead.enable=false";
    }

    AppLaunchEventState initial_state{&perfetto_factory_,
                                      readahead_allowed_,
                                      tracing_allowed_,
                                      &worker_thread2_,
                                      &io_thread_,
                                      &async_pool_};
    app_launch_events_
      .subscribe_on(worker_thread_)
      .scan(std::move(initial_state),
            [](AppLaunchEventState state, AppLaunchEventRefWrapper event) {
              state.OnNewEvent(event.get());
              return state;
            })
      .subscribe(/*out*/lifetime, [](const AppLaunchEventState& state) {
                   // Intentionally left blank.
                   (void)state;
                 });

    return lifetime;
  }

  rxcpp::composite_subscription InitializeRxGraphForJobScheduledEvents() {
    LOG(VERBOSE) << "EventManager::InitializeRxGraphForJobScheduledEvents";

    using RequestAndJobEvent = std::pair<RequestId, JobScheduledEvent>;

    job_scheduled_events_ = rxcpp::observable<>::create<RequestAndJobEvent>(
      [&](rxcpp::subscriber<RequestAndJobEvent> subscriber) {
        job_scheduled_event_subject_.Subscribe(std::move(subscriber));
      });

    rxcpp::composite_subscription lifetime;

    job_scheduled_events_
      .observe_on(worker_thread_)  // async handling.
      .tap([this](const RequestAndJobEvent& e) {
        LOG(VERBOSE) << "EventManager#JobScheduledEvent#tap(1) - job begins";
        this->NotifyProgress(e.first, TaskResult{TaskResult::State::kBegan});

        // TODO: probably this shouldn't be emitted until most of the usual DCHECKs
        // (for example, validate a job isn't already started, the request is not reused, etc).
        // In this way we could block from the client until it sees 'kBegan' and Log.wtf otherwise.
      })
      .tap([](const RequestAndJobEvent& e) {
        // TODO. Actual work.
        LOG(VERBOSE) << "EventManager#JobScheduledEvent#tap(2) - job is being processed";

        // TODO: abort functionality for in-flight jobs.
        //
        // maybe something like scan that returns an observable<Job> + flat map to that job.
        // then we could unsubscribe from the scan to do a partial abort? need to try it and see if it works.
        //
        // other option is to create a new outer subscription for each job id which seems less ideal.
      })
      .subscribe(/*out*/lifetime,
        /*on_next*/
        [this](const RequestAndJobEvent& e) {
          LOG(VERBOSE) << "EventManager#JobScheduledEvent#subscribe - job completed";
          this->NotifyComplete(e.first, TaskResult{TaskResult::State::kCompleted});
        }
#if 0
        ,
        /*on_error*/
        [](rxcpp::util::error_ptr err) {
          LOG(ERROR) << "Scheduled job event failed: " << rxcpp::util::what(err);

          //std::shared_ptr<TaskResultCallbacks> callbacks = callbacks_.lock();
          //if (callbacks != nullptr) {
            // FIXME: How do we get the request ID back out of the error? Seems like a problem.
            // callbacks->OnComplete(, TaskResult{TaskResult::kError});
            // We may have to wrap with an iorap::expected instead of using on_error.
          //}

          // FIXME: need to add a 'OnErrorResumeNext' operator?
          DCHECK(false) << "forgot to implement OnErrorResumeNext";
        }
#endif
      );

    // TODO: error output should happen via an observable.

    return lifetime;
  }

  void NotifyComplete(RequestId request_id, TaskResult result) {
      std::shared_ptr<TaskResultCallbacks> callbacks = callbacks_.lock();
      if (callbacks != nullptr) {
        callbacks->OnComplete(std::move(request_id), std::move(result));
      } else {
        LOG(WARNING) << "EventManager: TaskResultCallbacks may have been released early";
      }
  }

  void NotifyProgress(RequestId request_id, TaskResult result) {
      std::shared_ptr<TaskResultCallbacks> callbacks = callbacks_.lock();
      if (callbacks != nullptr) {
        callbacks->OnProgress(std::move(request_id), std::move(result));
      } else {
        LOG(WARNING) << "EventManager: TaskResultCallbacks may have been released early";
      }
  }

  bool readahead_allowed_{true};

  perfetto::RxProducerFactory& perfetto_factory_;
  bool tracing_allowed_{true};

  std::weak_ptr<TaskResultCallbacks> callbacks_;  // avoid cycles with weakptr.

  using AppLaunchEventRefWrapper = AppLaunchEventSubject::RefWrapper;
  rxcpp::observable<AppLaunchEventRefWrapper> app_launch_events_;
  AppLaunchEventSubject app_launch_event_subject_;

  rxcpp::observable<std::pair<RequestId, JobScheduledEvent>> job_scheduled_events_;
  JobScheduledEventSubject job_scheduled_event_subject_;

  rxcpp::observable<RequestId> completed_requests_;

  // regular-priority thread to handle binder callbacks.
  observe_on_one_worker worker_thread_;
  observe_on_one_worker worker_thread2_;
  // low priority idle-class thread for IO operations.
  observe_on_one_worker io_thread_;
  // async futures pool for async rx operations.
  AsyncPool async_pool_;

  rxcpp::composite_subscription rx_lifetime_;  // app launch events
  rxcpp::composite_subscription rx_lifetime_jobs_;  // job scheduled events

//INTENTIONAL_COMPILER_ERROR_HERE:
  // FIXME:
  // ok so we want to expose a 'BlockingSubscribe' or a 'Subscribe' or some kind of function
  // that the main thread can call. This would subscribe on all the observables we internally
  // have here (probably on an event-manager-dedicated thread for simplicity).
  //
  // ideally we'd just reuse the binder thread to handle the events but I'm not super sure,
  // maybe this already works with the identity_current_thread coordination?
};
using Impl = EventManager::Impl;

EventManager::EventManager(perfetto::RxProducerFactory& perfetto_factory)
    : impl_(new Impl(perfetto_factory)) {}

std::shared_ptr<EventManager> EventManager::Create() {
  static perfetto::PerfettoDependencies::Injector injector{
    perfetto::PerfettoDependencies::CreateComponent
  };
  static perfetto::RxProducerFactory producer_factory{
    /*borrow*/injector
  };
  return EventManager::Create(/*borrow*/producer_factory);
}

std::shared_ptr<EventManager> EventManager::Create(perfetto::RxProducerFactory& perfetto_factory) {
  std::shared_ptr<EventManager> p{new EventManager{/*borrow*/perfetto_factory}};
  return p;
}

void EventManager::SetTaskResultCallbacks(std::shared_ptr<TaskResultCallbacks> callbacks) {
  return impl_->SetTaskResultCallbacks(std::move(callbacks));
}

void EventManager::Join() {
  return impl_->Join();
}

bool EventManager::OnAppLaunchEvent(RequestId request_id,
                                    const AppLaunchEvent& event) {
  return impl_->OnAppLaunchEvent(request_id, event);
}

bool EventManager::OnJobScheduledEvent(RequestId request_id,
                                       const JobScheduledEvent& event) {
  return impl_->OnJobScheduledEvent(request_id, event);
}

}  // namespace iorap::manager
