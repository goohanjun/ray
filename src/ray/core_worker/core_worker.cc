#include "ray/core_worker/core_worker.h"

#include "boost/fiber/all.hpp"
#include "ray/common/ray_config.h"
#include "ray/common/task/task_util.h"
#include "ray/core_worker/context.h"
#include "ray/core_worker/transport/direct_actor_transport.h"
#include "ray/core_worker/transport/raylet_transport.h"
#include "ray/util/util.h"

namespace {

// Duration between internal book-keeping heartbeats.
const int kInternalHeartbeatMillis = 1000;

void BuildCommonTaskSpec(
    ray::TaskSpecBuilder &builder, const JobID &job_id, const TaskID &task_id,
    const TaskID &current_task_id, const int task_index, const TaskID &caller_id,
    const ray::rpc::Address &address, const ray::RayFunction &function,
    const std::vector<ray::TaskArg> &args, uint64_t num_returns,
    const std::unordered_map<std::string, double> &required_resources,
    const std::unordered_map<std::string, double> &required_placement_resources,
    ray::TaskTransportType transport_type, std::vector<ObjectID> *return_ids) {
  // Build common task spec.
  builder.SetCommonTaskSpec(task_id, function.GetLanguage(),
                            function.GetFunctionDescriptor(), job_id, current_task_id,
                            task_index, caller_id, address, num_returns,
                            transport_type == ray::TaskTransportType::DIRECT,
                            required_resources, required_placement_resources);
  // Set task arguments.
  for (const auto &arg : args) {
    if (arg.IsPassedByReference()) {
      builder.AddByRefArg(arg.GetReference());
    } else {
      builder.AddByValueArg(arg.GetValue());
    }
  }

  // Compute return IDs.
  return_ids->resize(num_returns);
  for (size_t i = 0; i < num_returns; i++) {
    (*return_ids)[i] =
        ObjectID::ForTaskReturn(task_id, i + 1,
                                /*transport_type=*/static_cast<int>(transport_type));
  }
}

// Group object ids according the the corresponding store providers.
void GroupObjectIdsByStoreProvider(const std::vector<ObjectID> &object_ids,
                                   absl::flat_hash_set<ObjectID> *plasma_object_ids,
                                   absl::flat_hash_set<ObjectID> *memory_object_ids) {
  for (const auto &object_id : object_ids) {
    if (object_id.IsDirectCallType()) {
      memory_object_ids->insert(object_id);
    } else {
      plasma_object_ids->insert(object_id);
    }
  }
}

}  // namespace

namespace ray {

CoreWorker::CoreWorker(const WorkerType worker_type, const Language language,
                       const std::string &store_socket, const std::string &raylet_socket,
                       const JobID &job_id, const gcs::GcsClientOptions &gcs_options,
                       const std::string &log_dir, const std::string &node_ip_address,
                       int node_manager_port,
                       const TaskExecutionCallback &task_execution_callback,
                       std::function<Status()> check_signals, bool ref_counting_enabled)
    : worker_type_(worker_type),
      language_(language),
      log_dir_(log_dir),
      ref_counting_enabled_(ref_counting_enabled),
      check_signals_(check_signals),
      worker_context_(worker_type, job_id),
      io_work_(io_service_),
      client_call_manager_(new rpc::ClientCallManager(io_service_)),
      death_check_timer_(io_service_),
      internal_timer_(io_service_),
      core_worker_server_(WorkerTypeString(worker_type), 0 /* let grpc choose a port */),
      reference_counter_(std::make_shared<ReferenceCounter>()),
      task_queue_length_(0),
      num_executed_tasks_(0),
      task_execution_service_work_(task_execution_service_),
      task_execution_callback_(task_execution_callback),
      resource_ids_(new ResourceMappingType()),
      grpc_service_(io_service_, *this) {
  // Initialize logging if log_dir is passed. Otherwise, it must be initialized
  // and cleaned up by the caller.
  if (log_dir_ != "") {
    std::stringstream app_name;
    app_name << LanguageString(language_) << "-" << WorkerTypeString(worker_type_) << "-"
             << worker_context_.GetWorkerID();
    RayLog::StartRayLog(app_name.str(), RayLogLevel::INFO, log_dir_);
    RayLog::InstallFailureSignalHandler();
  }
  RAY_LOG(INFO) << "Initializing worker " << worker_context_.GetWorkerID();

  // Initialize gcs client.
  gcs_client_ = std::make_shared<gcs::RedisGcsClient>(gcs_options);
  RAY_CHECK_OK(gcs_client_->Connect(io_service_));

  actor_manager_ = std::unique_ptr<ActorManager>(new ActorManager(gcs_client_->Actors()));

  // Initialize profiler.
  profiler_ = std::make_shared<worker::Profiler>(worker_context_, node_ip_address,
                                                 io_service_, gcs_client_);

  // Initialize task receivers.
  if (worker_type_ == WorkerType::WORKER) {
    RAY_CHECK(task_execution_callback_ != nullptr);
    auto execute_task = std::bind(&CoreWorker::ExecuteTask, this, std::placeholders::_1,
                                  std::placeholders::_2, std::placeholders::_3);
    auto exit = [this](bool intentional) {
      // Release the resources early in case draining takes a long time.
      RAY_CHECK_OK(local_raylet_client_->NotifyDirectCallTaskBlocked());
      task_manager_->DrainAndShutdown([this, intentional]() {
        // To avoid problems, make sure shutdown is always called from the same
        // event loop each time.
        task_execution_service_.post([this, intentional]() {
          if (intentional) {
            Disconnect();  // Notify the raylet this is an intentional exit.
          }
          Shutdown();
        });
      });
    };
    raylet_task_receiver_ =
        std::unique_ptr<CoreWorkerRayletTaskReceiver>(new CoreWorkerRayletTaskReceiver(
            worker_context_.GetWorkerID(), local_raylet_client_, execute_task, exit));
    direct_task_receiver_ = std::unique_ptr<CoreWorkerDirectTaskReceiver>(
        new CoreWorkerDirectTaskReceiver(worker_context_, local_raylet_client_,
                                         task_execution_service_, execute_task, exit));
  }

  // Start RPC server after all the task receivers are properly initialized.
  core_worker_server_.RegisterService(grpc_service_);
  core_worker_server_.Run();

  // Initialize raylet client.
  // TODO(zhijunfu): currently RayletClient would crash in its constructor if it cannot
  // connect to Raylet after a number of retries, this can be changed later
  // so that the worker (java/python .etc) can retrieve and handle the error
  // instead of crashing.
  auto grpc_client = rpc::NodeManagerWorkerClient::make(
      node_ip_address, node_manager_port, *client_call_manager_);
  ClientID local_raylet_id;
  local_raylet_client_ = std::shared_ptr<raylet::RayletClient>(new raylet::RayletClient(
      io_service_, std::move(grpc_client), raylet_socket, worker_context_.GetWorkerID(),
      (worker_type_ == ray::WorkerType::WORKER), worker_context_.GetCurrentJobID(),
      language_, &local_raylet_id, core_worker_server_.GetPort()));
  connected_ = true;

  // Set our own address.
  RAY_CHECK(!local_raylet_id.IsNil());
  rpc_address_.set_ip_address(node_ip_address);
  rpc_address_.set_port(core_worker_server_.GetPort());
  rpc_address_.set_raylet_id(local_raylet_id.Binary());
  rpc_address_.set_worker_id(worker_context_.GetWorkerID().Binary());

  if (worker_type_ == ray::WorkerType::WORKER) {
    death_check_timer_.expires_from_now(boost::asio::chrono::milliseconds(
        RayConfig::instance().raylet_death_check_interval_milliseconds()));
    death_check_timer_.async_wait(boost::bind(&CoreWorker::CheckForRayletFailure, this));
  }

  internal_timer_.expires_from_now(
      boost::asio::chrono::milliseconds(kInternalHeartbeatMillis));
  internal_timer_.async_wait(boost::bind(&CoreWorker::InternalHeartbeat, this));

  io_thread_ = std::thread(&CoreWorker::RunIOService, this);

  plasma_store_provider_.reset(new CoreWorkerPlasmaStoreProvider(
      store_socket, local_raylet_client_, check_signals_));
  memory_store_.reset(new CoreWorkerMemoryStore(
      [this](const RayObject &obj, const ObjectID &obj_id) {
        RAY_CHECK_OK(plasma_store_provider_->Put(obj, obj_id));
      },
      ref_counting_enabled ? reference_counter_ : nullptr, local_raylet_client_,
      check_signals_));

  task_manager_.reset(new TaskManager(
      memory_store_, reference_counter_, actor_manager_,
      [this](const TaskSpecification &spec) {
        // Retry after a delay to emulate the existing Raylet reconstruction
        // behaviour. TODO(ekl) backoff exponentially.
        RAY_LOG(ERROR) << "Will resubmit task after a 5 second delay: "
                       << spec.DebugString();
        absl::MutexLock lock(&mutex_);
        to_resubmit_.push_back(std::make_pair(current_time_ms() + 5000, spec));
      }));

  // Create an entry for the driver task in the task table. This task is
  // added immediately with status RUNNING. This allows us to push errors
  // related to this driver task back to the driver. For example, if the
  // driver creates an object that is later evicted, we should notify the
  // user that we're unable to reconstruct the object, since we cannot
  // rerun the driver.
  if (worker_type_ == WorkerType::DRIVER) {
    TaskSpecBuilder builder;
    std::vector<std::string> empty_descriptor;
    std::unordered_map<std::string, double> empty_resources;
    const TaskID task_id = TaskID::ForDriverTask(worker_context_.GetCurrentJobID());
    builder.SetCommonTaskSpec(
        task_id, language_, empty_descriptor, worker_context_.GetCurrentJobID(),
        TaskID::ComputeDriverTaskId(worker_context_.GetWorkerID()), 0, GetCallerId(),
        rpc_address_, 0, false, empty_resources, empty_resources);

    std::shared_ptr<gcs::TaskTableData> data = std::make_shared<gcs::TaskTableData>();
    data->mutable_task()->mutable_task_spec()->CopyFrom(builder.Build().GetMessage());
    RAY_CHECK_OK(gcs_client_->Tasks().AsyncAdd(data, nullptr));
    SetCurrentTaskId(task_id);
  }

  auto client_factory = [this](const std::string ip_address, int port) {
    return std::shared_ptr<rpc::CoreWorkerClient>(
        new rpc::CoreWorkerClient(ip_address, port, *client_call_manager_));
  };
  direct_actor_submitter_ = std::unique_ptr<CoreWorkerDirectActorTaskSubmitter>(
      new CoreWorkerDirectActorTaskSubmitter(rpc_address_, client_factory, memory_store_,
                                             task_manager_));

  direct_task_submitter_ =
      std::unique_ptr<CoreWorkerDirectTaskSubmitter>(new CoreWorkerDirectTaskSubmitter(
          rpc_address_, local_raylet_client_, client_factory,
          [this](const std::string ip_address, int port) {
            auto grpc_client = rpc::NodeManagerWorkerClient::make(ip_address, port,
                                                                  *client_call_manager_);
            return std::shared_ptr<raylet::RayletClient>(
                new raylet::RayletClient(std::move(grpc_client)));
          },
          memory_store_, task_manager_, local_raylet_id,
          RayConfig::instance().worker_lease_timeout_milliseconds()));
  future_resolver_.reset(new FutureResolver(memory_store_, client_factory));
  // Unfortunately the raylet client has to be constructed after the receivers.
  if (direct_task_receiver_ != nullptr) {
    direct_task_receiver_->Init(client_factory, rpc_address_);
  }
}

CoreWorker::~CoreWorker() {
  io_service_.stop();
  io_thread_.join();
  if (log_dir_ != "") {
    RayLog::ShutDownRayLog();
  }
}

void CoreWorker::Shutdown() {
  io_service_.stop();
  if (worker_type_ == WorkerType::WORKER) {
    task_execution_service_.stop();
  }
}

void CoreWorker::Disconnect() {
  io_service_.stop();
  if (connected_) {
    connected_ = false;
    if (gcs_client_) {
      gcs_client_->Disconnect();
    }
    if (local_raylet_client_) {
      RAY_IGNORE_EXPR(local_raylet_client_->Disconnect());
    }
  }
}

void CoreWorker::RunIOService() {
#ifdef _WIN32
  // TODO(mehrdadn): Is there an equivalent for Windows we need here?
#else
  // Block SIGINT and SIGTERM so they will be handled by the main thread.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &mask, NULL);
#endif

  io_service_.run();
}

void CoreWorker::SetCurrentTaskId(const TaskID &task_id) {
  worker_context_.SetCurrentTaskId(task_id);
  main_thread_task_id_ = task_id;
  bool not_actor_task = false;
  {
    absl::MutexLock lock(&mutex_);
    not_actor_task = actor_id_.IsNil();
  }
  // Clear all actor handles at the end of each non-actor task.
  if (not_actor_task && task_id.IsNil()) {
    absl::MutexLock lock(&actor_handles_mutex_);
    for (const auto &handle : actor_handles_) {
      RAY_CHECK_OK(gcs_client_->Actors().AsyncUnsubscribe(handle.first, nullptr));
    }
    actor_handles_.clear();
  }
}

void CoreWorker::CheckForRayletFailure() {
// If the raylet fails, we will be reassigned to init (PID=1).
#ifdef _WIN32
// TODO(mehrdadn): need a different solution for Windows.
#else
  if (getppid() == 1) {
    RAY_LOG(ERROR) << "Raylet failed. Shutting down.";
    Shutdown();
  }
#endif

  // Reset the timer from the previous expiration time to avoid drift.
  death_check_timer_.expires_at(
      death_check_timer_.expiry() +
      boost::asio::chrono::milliseconds(
          RayConfig::instance().raylet_death_check_interval_milliseconds()));
  death_check_timer_.async_wait(boost::bind(&CoreWorker::CheckForRayletFailure, this));
}

void CoreWorker::InternalHeartbeat() {
  absl::MutexLock lock(&mutex_);
  while (!to_resubmit_.empty() && current_time_ms() > to_resubmit_.front().first) {
    RAY_CHECK_OK(direct_task_submitter_->SubmitTask(to_resubmit_.front().second));
    to_resubmit_.pop_front();
  }
  internal_timer_.expires_at(internal_timer_.expiry() +
                             boost::asio::chrono::milliseconds(kInternalHeartbeatMillis));
  internal_timer_.async_wait(boost::bind(&CoreWorker::InternalHeartbeat, this));
}

void CoreWorker::PromoteToPlasmaAndGetOwnershipInfo(const ObjectID &object_id,
                                                    TaskID *owner_id,
                                                    rpc::Address *owner_address) {
  RAY_CHECK(object_id.IsDirectCallType());
  auto value = memory_store_->GetOrPromoteToPlasma(object_id);
  if (value) {
    RAY_CHECK_OK(plasma_store_provider_->Put(*value, object_id));
  }

  auto has_owner = reference_counter_->GetOwner(object_id, owner_id, owner_address);
  RAY_CHECK(has_owner)
      << "Object IDs generated randomly (ObjectID.from_random()) or out-of-band "
         "(ObjectID.from_binary(...)) cannot be serialized because Ray does not know "
         "which task will create them. "
         "If this was not how your object ID was generated, please file an issue "
         "at https://github.com/ray-project/ray/issues/";
}

void CoreWorker::RegisterOwnershipInfoAndResolveFuture(
    const ObjectID &object_id, const TaskID &owner_id,
    const rpc::Address &owner_address) {
  // Add the object's owner to the local metadata in case it gets serialized
  // again.
  reference_counter_->AddBorrowedObject(object_id, owner_id, owner_address);

  RAY_CHECK(!owner_id.IsNil());
  // We will ask the owner about the object until the object is
  // created or we can no longer reach the owner.
  future_resolver_->ResolveFutureAsync(object_id, owner_id, owner_address);
}

void CoreWorker::AddContainedObjectIDs(
    const ObjectID &object_id, const std::vector<ObjectID> &contained_object_ids) {
  // TODO(edoakes,swang): integrate with the reference counting logic.
}

Status CoreWorker::SetClientOptions(std::string name, int64_t limit_bytes) {
  // Currently only the Plasma store supports client options.
  return plasma_store_provider_->SetClientOptions(name, limit_bytes);
}

Status CoreWorker::Put(const RayObject &object,
                       const std::vector<ObjectID> &contained_object_ids,
                       ObjectID *object_id) {
  *object_id = ObjectID::ForPut(worker_context_.GetCurrentTaskID(),
                                worker_context_.GetNextPutIndex(),
                                static_cast<uint8_t>(TaskTransportType::RAYLET));
  reference_counter_->AddOwnedObject(*object_id, GetCallerId(), rpc_address_);
  RAY_RETURN_NOT_OK(Put(object, contained_object_ids, *object_id));
  // Tell the raylet to pin the object **after** it is created.
  RAY_CHECK_OK(local_raylet_client_->PinObjectIDs(rpc_address_, {*object_id}));
  return Status::OK();
}

Status CoreWorker::Put(const RayObject &object,
                       const std::vector<ObjectID> &contained_object_ids,
                       const ObjectID &object_id) {
  RAY_CHECK(object_id.GetTransportType() ==
            static_cast<uint8_t>(TaskTransportType::RAYLET))
      << "Invalid transport type flag in object ID: " << object_id.GetTransportType();
  // TODO(edoakes,swang): add contained object IDs to the reference counter.
  return plasma_store_provider_->Put(object, object_id);
}

Status CoreWorker::Create(const std::shared_ptr<Buffer> &metadata, const size_t data_size,
                          const std::vector<ObjectID> &contained_object_ids,
                          ObjectID *object_id, std::shared_ptr<Buffer> *data) {
  *object_id = ObjectID::ForPut(worker_context_.GetCurrentTaskID(),
                                worker_context_.GetNextPutIndex(),
                                static_cast<uint8_t>(TaskTransportType::RAYLET));
  RAY_RETURN_NOT_OK(Create(metadata, data_size, contained_object_ids, *object_id, data));
  // Only add the object to the reference counter if it didn't already exist.
  if (data) {
    reference_counter_->AddOwnedObject(*object_id, GetCallerId(), rpc_address_);
  }
  return Status::OK();
}

Status CoreWorker::Create(const std::shared_ptr<Buffer> &metadata, const size_t data_size,
                          const std::vector<ObjectID> &contained_object_ids,
                          const ObjectID &object_id, std::shared_ptr<Buffer> *data) {
  // TODO(edoakes,swang): add contained object IDs to the reference counter.
  return plasma_store_provider_->Create(metadata, data_size, object_id, data);
}

Status CoreWorker::Seal(const ObjectID &object_id, bool pin_object) {
  RAY_RETURN_NOT_OK(plasma_store_provider_->Seal(object_id));
  if (pin_object) {
    // Tell the raylet to pin the object **after** it is created.
    RAY_CHECK_OK(local_raylet_client_->PinObjectIDs(rpc_address_, {object_id}));
  }
  return Status::OK();
}

Status CoreWorker::Get(const std::vector<ObjectID> &ids, const int64_t timeout_ms,
                       std::vector<std::shared_ptr<RayObject>> *results) {
  results->resize(ids.size(), nullptr);

  absl::flat_hash_set<ObjectID> plasma_object_ids;
  absl::flat_hash_set<ObjectID> memory_object_ids;
  GroupObjectIdsByStoreProvider(ids, &plasma_object_ids, &memory_object_ids);

  bool got_exception = false;
  absl::flat_hash_map<ObjectID, std::shared_ptr<RayObject>> result_map;
  auto start_time = current_time_ms();

  if (!memory_object_ids.empty()) {
    RAY_RETURN_NOT_OK(memory_store_->Get(memory_object_ids, timeout_ms, worker_context_,
                                         &result_map, &got_exception));
  }

  if (!got_exception) {
    // If any of the objects have been promoted to plasma, then we retry their
    // gets at the provider plasma. Once we get the objects from plasma, we flip
    // the transport type again and return them for the original direct call ids.
    for (const auto &pair : result_map) {
      if (pair.second->IsInPlasmaError()) {
        RAY_LOG(INFO) << pair.first << " in plasma, doing fetch-and-get";
        plasma_object_ids.insert(pair.first);
      }
    }
    int64_t local_timeout_ms = timeout_ms;
    if (timeout_ms >= 0) {
      local_timeout_ms = std::max(static_cast<int64_t>(0),
                                  timeout_ms - (current_time_ms() - start_time));
    }
    RAY_LOG(DEBUG) << "Plasma GET timeout " << local_timeout_ms;
    RAY_RETURN_NOT_OK(plasma_store_provider_->Get(plasma_object_ids, local_timeout_ms,
                                                  worker_context_, &result_map,
                                                  &got_exception));
  }

  // Loop through `ids` and fill each entry for the `results` vector,
  // this ensures that entries `results` have exactly the same order as
  // they are in `ids`. When there are duplicate object ids, all the entries
  // for the same id are filled in.
  bool missing_result = false;
  bool will_throw_exception = false;
  for (size_t i = 0; i < ids.size(); i++) {
    auto pair = result_map.find(ids[i]);
    if (pair != result_map.end()) {
      (*results)[i] = pair->second;
      RAY_CHECK(!pair->second->IsInPlasmaError());
      if (pair->second->IsException()) {
        // The language bindings should throw an exception if they see this
        // object.
        will_throw_exception = true;
      }
    } else {
      missing_result = true;
    }
  }
  // If no timeout was set and none of the results will throw an exception,
  // then check that we fetched all results before returning.
  if (timeout_ms < 0 && !will_throw_exception) {
    RAY_CHECK(!missing_result);
  }

  return Status::OK();
}

Status CoreWorker::Contains(const ObjectID &object_id, bool *has_object) {
  bool found = false;
  if (object_id.IsDirectCallType()) {
    bool in_plasma = false;
    found = memory_store_->Contains(object_id, &in_plasma);
    if (in_plasma) {
      RAY_RETURN_NOT_OK(plasma_store_provider_->Contains(object_id, &found));
    }
  } else {
    RAY_RETURN_NOT_OK(plasma_store_provider_->Contains(object_id, &found));
  }
  *has_object = found;
  return Status::OK();
}

// For any objects that are ErrorType::OBJECT_IN_PLASMA, we need to move them from
// the ready set into the plasma_object_ids set to wait on them there.
void RetryObjectInPlasmaErrors(std::shared_ptr<CoreWorkerMemoryStore> &memory_store,
                               WorkerContext &worker_context,
                               absl::flat_hash_set<ObjectID> &memory_object_ids,
                               absl::flat_hash_set<ObjectID> &plasma_object_ids,
                               absl::flat_hash_set<ObjectID> &ready) {
  for (const auto &mem_id : memory_object_ids) {
    if (ready.find(mem_id) != ready.end()) {
      std::vector<std::shared_ptr<RayObject>> found;
      RAY_CHECK_OK(memory_store->Get({mem_id}, /*num_objects=*/1, /*timeout=*/0,
                                     worker_context,
                                     /*remote_after_get=*/false, &found));
      if (found.size() == 1 && found[0]->IsInPlasmaError()) {
        memory_object_ids.erase(mem_id);
        ready.erase(mem_id);
        plasma_object_ids.insert(mem_id);
      }
    }
  }
}

Status CoreWorker::Wait(const std::vector<ObjectID> &ids, int num_objects,
                        int64_t timeout_ms, std::vector<bool> *results) {
  results->resize(ids.size(), false);

  if (num_objects <= 0 || num_objects > static_cast<int>(ids.size())) {
    return Status::Invalid(
        "Number of objects to wait for must be between 1 and the number of ids.");
  }

  absl::flat_hash_set<ObjectID> plasma_object_ids;
  absl::flat_hash_set<ObjectID> memory_object_ids;
  GroupObjectIdsByStoreProvider(ids, &plasma_object_ids, &memory_object_ids);

  if (plasma_object_ids.size() + memory_object_ids.size() != ids.size()) {
    return Status::Invalid("Duplicate object IDs not supported in wait.");
  }

  // TODO(edoakes): this logic is not ideal, and will have to be addressed
  // before we enable direct actor calls in the Python code. If we are waiting
  // on a list of objects mixed between multiple store providers, we could
  // easily end up in the situation where we're blocked waiting on one store
  // provider while another actually has enough objects ready to fulfill
  // 'num_objects'. This is partially addressed by trying them all once with
  // a timeout of 0, but that does not address the situation where objects
  // become available on the second store provider while waiting on the first.

  absl::flat_hash_set<ObjectID> ready;
  // Wait from both store providers with timeout set to 0. This is to avoid the case
  // where we might use up the entire timeout on trying to get objects from one store
  // provider before even trying another (which might have all of the objects available).
  if (memory_object_ids.size() > 0) {
    RAY_RETURN_NOT_OK(memory_store_->Wait(
        memory_object_ids,
        std::min(static_cast<int>(memory_object_ids.size()), num_objects),
        /*timeout_ms=*/0, worker_context_, &ready));
    RetryObjectInPlasmaErrors(memory_store_, worker_context_, memory_object_ids,
                              plasma_object_ids, ready);
  }
  RAY_CHECK(static_cast<int>(ready.size()) <= num_objects);
  if (static_cast<int>(ready.size()) < num_objects && plasma_object_ids.size() > 0) {
    RAY_RETURN_NOT_OK(plasma_store_provider_->Wait(
        plasma_object_ids,
        std::min(static_cast<int>(plasma_object_ids.size()),
                 num_objects - static_cast<int>(ready.size())),
        /*timeout_ms=*/0, worker_context_, &ready));
  }
  RAY_CHECK(static_cast<int>(ready.size()) <= num_objects);

  if (timeout_ms != 0 && static_cast<int>(ready.size()) < num_objects) {
    // Clear the ready set and retry. We clear it so that we can compute the number of
    // objects to fetch from the memory store easily below.
    ready.clear();

    int64_t start_time = current_time_ms();
    if (memory_object_ids.size() > 0) {
      RAY_RETURN_NOT_OK(memory_store_->Wait(
          memory_object_ids,
          std::min(static_cast<int>(memory_object_ids.size()), num_objects), timeout_ms,
          worker_context_, &ready));
      RetryObjectInPlasmaErrors(memory_store_, worker_context_, memory_object_ids,
                                plasma_object_ids, ready);
    }
    RAY_CHECK(static_cast<int>(ready.size()) <= num_objects);
    if (timeout_ms > 0) {
      timeout_ms =
          std::max(0, static_cast<int>(timeout_ms - (current_time_ms() - start_time)));
    }
    if (static_cast<int>(ready.size()) < num_objects && plasma_object_ids.size() > 0) {
      RAY_RETURN_NOT_OK(plasma_store_provider_->Wait(
          plasma_object_ids,
          std::min(static_cast<int>(plasma_object_ids.size()),
                   num_objects - static_cast<int>(ready.size())),
          timeout_ms, worker_context_, &ready));
    }
    RAY_CHECK(static_cast<int>(ready.size()) <= num_objects);
  }

  for (size_t i = 0; i < ids.size(); i++) {
    if (ready.find(ids[i]) != ready.end()) {
      results->at(i) = true;
    }
  }

  return Status::OK();
}

Status CoreWorker::Delete(const std::vector<ObjectID> &object_ids, bool local_only,
                          bool delete_creating_tasks) {
  absl::flat_hash_set<ObjectID> plasma_object_ids;
  absl::flat_hash_set<ObjectID> memory_object_ids;
  GroupObjectIdsByStoreProvider(object_ids, &plasma_object_ids, &memory_object_ids);

  // TODO(edoakes): what are the desired semantics for deleting from a non-owner?
  // Should we just delete locally or ping the owner and delete globally?
  reference_counter_->DeleteReferences(object_ids);
  memory_store_->Delete(memory_object_ids, &plasma_object_ids);
  RAY_RETURN_NOT_OK(plasma_store_provider_->Delete(plasma_object_ids, local_only,
                                                   delete_creating_tasks));

  return Status::OK();
}

std::string CoreWorker::MemoryUsageString() {
  // Currently only the Plasma store returns a debug string.
  return plasma_store_provider_->MemoryUsageString();
}

TaskID CoreWorker::GetCallerId() const {
  TaskID caller_id;
  ActorID actor_id = GetActorId();
  if (!actor_id.IsNil()) {
    caller_id = TaskID::ForActorCreationTask(actor_id);
  } else {
    caller_id = main_thread_task_id_;
  }
  return caller_id;
}

Status CoreWorker::PushError(const JobID &job_id, const std::string &type,
                             const std::string &error_message, double timestamp) {
  return local_raylet_client_->PushError(job_id, type, error_message, timestamp);
}

Status CoreWorker::PrepareActorCheckpoint(const ActorID &actor_id,
                                          ActorCheckpointID *checkpoint_id) {
  return local_raylet_client_->PrepareActorCheckpoint(actor_id, checkpoint_id);
}

Status CoreWorker::NotifyActorResumedFromCheckpoint(
    const ActorID &actor_id, const ActorCheckpointID &checkpoint_id) {
  return local_raylet_client_->NotifyActorResumedFromCheckpoint(actor_id, checkpoint_id);
}

Status CoreWorker::SetResource(const std::string &resource_name, const double capacity,
                               const ClientID &client_id) {
  return local_raylet_client_->SetResource(resource_name, capacity, client_id);
}

Status CoreWorker::SubmitTask(const RayFunction &function,
                              const std::vector<TaskArg> &args,
                              const TaskOptions &task_options,
                              std::vector<ObjectID> *return_ids, int max_retries) {
  TaskSpecBuilder builder;
  const int next_task_index = worker_context_.GetNextTaskIndex();
  const auto task_id =
      TaskID::ForNormalTask(worker_context_.GetCurrentJobID(),
                            worker_context_.GetCurrentTaskID(), next_task_index);

  const std::unordered_map<std::string, double> required_resources;
  // TODO(ekl) offload task building onto a thread pool for performance
  BuildCommonTaskSpec(
      builder, worker_context_.GetCurrentJobID(), task_id,
      worker_context_.GetCurrentTaskID(), next_task_index, GetCallerId(), rpc_address_,
      function, args, task_options.num_returns, task_options.resources,
      required_resources,
      task_options.is_direct_call ? TaskTransportType::DIRECT : TaskTransportType::RAYLET,
      return_ids);
  TaskSpecification task_spec = builder.Build();
  if (task_options.is_direct_call) {
    task_manager_->AddPendingTask(GetCallerId(), rpc_address_, task_spec, max_retries);
    return direct_task_submitter_->SubmitTask(task_spec);
  } else {
    return local_raylet_client_->SubmitTask(task_spec);
  }
}

Status CoreWorker::CreateActor(const RayFunction &function,
                               const std::vector<TaskArg> &args,
                               const ActorCreationOptions &actor_creation_options,
                               ActorID *return_actor_id) {
  const int next_task_index = worker_context_.GetNextTaskIndex();
  const ActorID actor_id =
      ActorID::Of(worker_context_.GetCurrentJobID(), worker_context_.GetCurrentTaskID(),
                  next_task_index);
  const TaskID actor_creation_task_id = TaskID::ForActorCreationTask(actor_id);
  const JobID job_id = worker_context_.GetCurrentJobID();
  std::vector<ObjectID> return_ids;
  TaskSpecBuilder builder;
  BuildCommonTaskSpec(builder, job_id, actor_creation_task_id,
                      worker_context_.GetCurrentTaskID(), next_task_index, GetCallerId(),
                      rpc_address_, function, args, 1, actor_creation_options.resources,
                      actor_creation_options.placement_resources,
                      actor_creation_options.is_direct_call ? TaskTransportType::DIRECT
                                                            : TaskTransportType::RAYLET,
                      &return_ids);
  builder.SetActorCreationTaskSpec(
      actor_id, actor_creation_options.max_reconstructions,
      actor_creation_options.dynamic_worker_options,
      actor_creation_options.is_direct_call, actor_creation_options.max_concurrency,
      actor_creation_options.is_detached, actor_creation_options.is_asyncio);

  std::unique_ptr<ActorHandle> actor_handle(new ActorHandle(
      actor_id, job_id, /*actor_cursor=*/return_ids[0], function.GetLanguage(),
      actor_creation_options.is_direct_call, function.GetFunctionDescriptor()));
  RAY_CHECK(AddActorHandle(std::move(actor_handle)))
      << "Actor " << actor_id << " already exists";

  *return_actor_id = actor_id;
  TaskSpecification task_spec = builder.Build();
  if (actor_creation_options.is_direct_call) {
    task_manager_->AddPendingTask(
        GetCallerId(), rpc_address_, task_spec,
        std::max(RayConfig::instance().actor_creation_min_retries(),
                 actor_creation_options.max_reconstructions));
    return direct_task_submitter_->SubmitTask(task_spec);
  } else {
    return local_raylet_client_->SubmitTask(task_spec);
  }
}

Status CoreWorker::SubmitActorTask(const ActorID &actor_id, const RayFunction &function,
                                   const std::vector<TaskArg> &args,
                                   const TaskOptions &task_options,
                                   std::vector<ObjectID> *return_ids) {
  ActorHandle *actor_handle = nullptr;
  RAY_RETURN_NOT_OK(GetActorHandle(actor_id, &actor_handle));

  // Add one for actor cursor object id for tasks.
  const int num_returns = task_options.num_returns + 1;

  const bool is_direct_call = actor_handle->IsDirectCallActor();
  const TaskTransportType transport_type =
      is_direct_call ? TaskTransportType::DIRECT : TaskTransportType::RAYLET;

  // Build common task spec.
  TaskSpecBuilder builder;
  const int next_task_index = worker_context_.GetNextTaskIndex();
  const TaskID actor_task_id = TaskID::ForActorTask(
      worker_context_.GetCurrentJobID(), worker_context_.GetCurrentTaskID(),
      next_task_index, actor_handle->GetActorID());
  const std::unordered_map<std::string, double> required_resources;
  BuildCommonTaskSpec(builder, actor_handle->CreationJobID(), actor_task_id,
                      worker_context_.GetCurrentTaskID(), next_task_index, GetCallerId(),
                      rpc_address_, function, args, num_returns, task_options.resources,
                      required_resources, transport_type, return_ids);

  const ObjectID new_cursor = return_ids->back();
  actor_handle->SetActorTaskSpec(builder, transport_type, new_cursor);
  // Remove cursor from return ids.
  return_ids->pop_back();

  // Submit task.
  Status status;
  TaskSpecification task_spec = builder.Build();
  if (is_direct_call) {
    task_manager_->AddPendingTask(GetCallerId(), rpc_address_, task_spec);
    if (actor_handle->IsDead()) {
      auto status = Status::IOError("sent task to dead actor");
      task_manager_->PendingTaskFailed(task_spec.TaskId(), rpc::ErrorType::ACTOR_DIED,
                                       &status);
    } else {
      status = direct_actor_submitter_->SubmitTask(task_spec);
    }
  } else {
    RAY_CHECK_OK(local_raylet_client_->SubmitTask(task_spec));
  }
  return status;
}

Status CoreWorker::KillActor(const ActorID &actor_id) {
  ActorHandle *actor_handle = nullptr;
  RAY_RETURN_NOT_OK(GetActorHandle(actor_id, &actor_handle));
  RAY_CHECK(actor_handle->IsDirectCallActor());
  return direct_actor_submitter_->KillActor(actor_id);
}

ActorID CoreWorker::DeserializeAndRegisterActorHandle(const std::string &serialized) {
  std::unique_ptr<ActorHandle> actor_handle(new ActorHandle(serialized));
  const ActorID actor_id = actor_handle->GetActorID();
  RAY_UNUSED(AddActorHandle(std::move(actor_handle)));
  return actor_id;
}

Status CoreWorker::SerializeActorHandle(const ActorID &actor_id,
                                        std::string *output) const {
  ActorHandle *actor_handle = nullptr;
  auto status = GetActorHandle(actor_id, &actor_handle);
  if (status.ok()) {
    actor_handle->Serialize(output);
  }
  return status;
}

bool CoreWorker::AddActorHandle(std::unique_ptr<ActorHandle> actor_handle) {
  absl::MutexLock lock(&actor_handles_mutex_);
  const auto &actor_id = actor_handle->GetActorID();

  auto inserted = actor_handles_.emplace(actor_id, std::move(actor_handle)).second;
  if (inserted) {
    // Register a callback to handle actor notifications.
    auto actor_notification_callback = [this](const ActorID &actor_id,
                                              const gcs::ActorTableData &actor_data) {
      if (actor_data.state() == gcs::ActorTableData::RECONSTRUCTING) {
        absl::MutexLock lock(&actor_handles_mutex_);
        auto it = actor_handles_.find(actor_id);
        RAY_CHECK(it != actor_handles_.end());
        if (it->second->IsDirectCallActor()) {
          // We have to reset the actor handle since the next instance of the
          // actor will not have the last sequence number that we sent.
          // TODO: Remove the check for direct calls. We do not reset for the
          // raylet codepath because it tries to replay all tasks since the
          // last actor checkpoint.
          it->second->Reset();
        }
        direct_actor_submitter_->DisconnectActor(actor_id, false);
      } else if (actor_data.state() == gcs::ActorTableData::DEAD) {
        direct_actor_submitter_->DisconnectActor(actor_id, true);

        ActorHandle *actor_handle = nullptr;
        RAY_CHECK_OK(GetActorHandle(actor_id, &actor_handle));
        actor_handle->MarkDead();
        // We cannot erase the actor handle here because clients can still
        // submit tasks to dead actors. This also means we defer unsubscription,
        // otherwise we crash when bulk unsubscribing all actor handles.
      } else {
        direct_actor_submitter_->ConnectActor(actor_id, actor_data.address());
      }

      RAY_LOG(INFO) << "received notification on actor, state="
                    << static_cast<int>(actor_data.state()) << ", actor_id: " << actor_id
                    << ", ip address: " << actor_data.address().ip_address()
                    << ", port: " << actor_data.address().port() << ", worker_id: "
                    << WorkerID::FromBinary(actor_data.address().worker_id())
                    << ", raylet_id: "
                    << ClientID::FromBinary(actor_data.address().raylet_id());
    };

    RAY_CHECK_OK(gcs_client_->Actors().AsyncSubscribe(
        actor_id, actor_notification_callback, nullptr));
  }
  return inserted;
}

Status CoreWorker::GetActorHandle(const ActorID &actor_id,
                                  ActorHandle **actor_handle) const {
  absl::MutexLock lock(&actor_handles_mutex_);
  auto it = actor_handles_.find(actor_id);
  if (it == actor_handles_.end()) {
    return Status::Invalid("Handle for actor does not exist");
  }
  *actor_handle = it->second.get();
  return Status::OK();
}

std::unique_ptr<worker::ProfileEvent> CoreWorker::CreateProfileEvent(
    const std::string &event_type) {
  return std::unique_ptr<worker::ProfileEvent>(
      new worker::ProfileEvent(profiler_, event_type));
}

void CoreWorker::StartExecutingTasks() { task_execution_service_.run(); }

Status CoreWorker::AllocateReturnObjects(
    const std::vector<ObjectID> &object_ids, const std::vector<size_t> &data_sizes,
    const std::vector<std::shared_ptr<Buffer>> &metadatas,
    const std::vector<std::vector<ObjectID>> &contained_object_ids,
    std::vector<std::shared_ptr<RayObject>> *return_objects) {
  RAY_CHECK(object_ids.size() == metadatas.size());
  RAY_CHECK(object_ids.size() == data_sizes.size());
  return_objects->resize(object_ids.size(), nullptr);

  for (size_t i = 0; i < object_ids.size(); i++) {
    bool object_already_exists = false;
    std::shared_ptr<Buffer> data_buffer;
    if (data_sizes[i] > 0) {
      if (worker_context_.CurrentTaskIsDirectCall() &&
          static_cast<int64_t>(data_sizes[i]) <
              RayConfig::instance().max_direct_call_object_size() &&
          contained_object_ids[i].empty()) {
        data_buffer = std::make_shared<LocalMemoryBuffer>(data_sizes[i]);
      } else {
        RAY_RETURN_NOT_OK(Create(metadatas[i], data_sizes[i], contained_object_ids[i],
                                 object_ids[i], &data_buffer));
        object_already_exists = !data_buffer;
      }
    }
    // Leave the return object as a nullptr if there is no data or metadata.
    // This allows the caller to prevent the core worker from storing an output
    // (e.g., to support ray.experimental.no_return.NoReturn).
    if (!object_already_exists && (data_buffer || metadatas[i])) {
      return_objects->at(i) = std::make_shared<RayObject>(data_buffer, metadatas[i]);
    }
  }

  return Status::OK();
}

Status CoreWorker::ExecuteTask(const TaskSpecification &task_spec,
                               const std::shared_ptr<ResourceMappingType> &resource_ids,
                               std::vector<std::shared_ptr<RayObject>> *return_objects) {
  task_queue_length_ -= 1;
  num_executed_tasks_ += 1;

  if (resource_ids != nullptr) {
    resource_ids_ = resource_ids;
  }
  worker_context_.SetCurrentTask(task_spec);
  SetCurrentTaskId(task_spec.TaskId());

  {
    absl::MutexLock lock(&mutex_);
    current_task_ = task_spec;
  }

  RayFunction func{task_spec.GetLanguage(), task_spec.FunctionDescriptor()};

  std::vector<std::shared_ptr<RayObject>> args;
  std::vector<ObjectID> arg_reference_ids;
  RAY_CHECK_OK(BuildArgsForExecutor(task_spec, &args, &arg_reference_ids));

  const auto transport_type = worker_context_.CurrentTaskIsDirectCall()
                                  ? TaskTransportType::DIRECT
                                  : TaskTransportType::RAYLET;
  std::vector<ObjectID> return_ids;
  for (size_t i = 0; i < task_spec.NumReturns(); i++) {
    return_ids.push_back(task_spec.ReturnId(i, transport_type));
  }

  Status status;
  TaskType task_type = TaskType::NORMAL_TASK;
  if (task_spec.IsActorCreationTask()) {
    RAY_CHECK(return_ids.size() > 0);
    return_ids.pop_back();
    task_type = TaskType::ACTOR_CREATION_TASK;
    SetActorId(task_spec.ActorCreationId());
    RAY_LOG(INFO) << "Creating actor: " << task_spec.ActorCreationId();
  } else if (task_spec.IsActorTask()) {
    RAY_CHECK(return_ids.size() > 0);
    return_ids.pop_back();
    task_type = TaskType::ACTOR_TASK;
  }

  status = task_execution_callback_(task_type, func,
                                    task_spec.GetRequiredResources().GetResourceMap(),
                                    args, arg_reference_ids, return_ids, return_objects);

  for (size_t i = 0; i < return_objects->size(); i++) {
    // The object is nullptr if it already existed in the object store.
    if (!return_objects->at(i)) {
      continue;
    }
    if (return_objects->at(i)->GetData()->IsPlasmaBuffer()) {
      if (!Seal(return_ids[i], /*pin_object=*/false).ok()) {
        RAY_LOG(FATAL) << "Task " << task_spec.TaskId() << " failed to seal object "
                       << return_ids[i] << " in store: " << status.message();
      }
    } else if (!worker_context_.CurrentTaskIsDirectCall()) {
      if (!Put(*return_objects->at(i), {}, return_ids[i]).ok()) {
        RAY_LOG(FATAL) << "Task " << task_spec.TaskId() << " failed to put object "
                       << return_ids[i] << " in store: " << status.message();
      }
    }
  }

  if (task_spec.IsNormalTask() && reference_counter_->NumObjectIDsInScope() != 0) {
    RAY_LOG(DEBUG)
        << "There were " << reference_counter_->NumObjectIDsInScope()
        << " ObjectIDs left in scope after executing task " << task_spec.TaskId()
        << ". This is either caused by keeping references to ObjectIDs in Python between "
           "tasks (e.g., in global variables) or indicates a problem with Ray's "
           "reference counting, and may cause problems in the object store.";
  }

  SetCurrentTaskId(TaskID::Nil());
  worker_context_.ResetCurrentTask(task_spec);
  {
    absl::MutexLock lock(&mutex_);
    current_task_ = TaskSpecification();
  }
  return status;
}

Status CoreWorker::BuildArgsForExecutor(const TaskSpecification &task,
                                        std::vector<std::shared_ptr<RayObject>> *args,
                                        std::vector<ObjectID> *arg_reference_ids) {
  auto num_args = task.NumArgs();
  args->resize(num_args);
  arg_reference_ids->resize(num_args);

  absl::flat_hash_set<ObjectID> by_ref_ids;
  absl::flat_hash_map<ObjectID, int> by_ref_indices;

  for (size_t i = 0; i < task.NumArgs(); ++i) {
    int count = task.ArgIdCount(i);
    if (count > 0) {
      // pass by reference.
      RAY_CHECK(count == 1);
      // Direct call type objects that weren't inlined have been promoted to plasma.
      // We need to put an OBJECT_IN_PLASMA error here so the subsequent call to Get()
      // properly redirects to the plasma store.
      if (task.ArgId(i, 0).IsDirectCallType()) {
        RAY_CHECK_OK(memory_store_->Put(RayObject(rpc::ErrorType::OBJECT_IN_PLASMA),
                                        task.ArgId(i, 0)));
      }
      by_ref_ids.insert(task.ArgId(i, 0));
      by_ref_indices.emplace(task.ArgId(i, 0), i);
      arg_reference_ids->at(i) = task.ArgId(i, 0);
    } else {
      // pass by value.
      std::shared_ptr<LocalMemoryBuffer> data = nullptr;
      if (task.ArgDataSize(i)) {
        data = std::make_shared<LocalMemoryBuffer>(const_cast<uint8_t *>(task.ArgData(i)),
                                                   task.ArgDataSize(i));
      }
      std::shared_ptr<LocalMemoryBuffer> metadata = nullptr;
      if (task.ArgMetadataSize(i)) {
        metadata = std::make_shared<LocalMemoryBuffer>(
            const_cast<uint8_t *>(task.ArgMetadata(i)), task.ArgMetadataSize(i));
      }
      args->at(i) = std::make_shared<RayObject>(data, metadata, /*copy_data*/ true);
      arg_reference_ids->at(i) = ObjectID::Nil();
    }
  }

  // Fetch by-reference arguments directly from the plasma store.
  bool got_exception = false;
  absl::flat_hash_map<ObjectID, std::shared_ptr<RayObject>> result_map;
  RAY_RETURN_NOT_OK(plasma_store_provider_->Get(by_ref_ids, -1, worker_context_,
                                                &result_map, &got_exception));
  for (const auto &it : result_map) {
    args->at(by_ref_indices[it.first]) = it.second;
  }

  return Status::OK();
}

void CoreWorker::HandleAssignTask(const rpc::AssignTaskRequest &request,
                                  rpc::AssignTaskReply *reply,
                                  rpc::SendReplyCallback send_reply_callback) {
  if (HandleWrongRecipient(WorkerID::FromBinary(request.intended_worker_id()),
                           send_reply_callback)) {
    return;
  }

  if (worker_context_.CurrentActorIsDirectCall()) {
    send_reply_callback(Status::Invalid("This actor only accepts direct calls."), nullptr,
                        nullptr);
    return;
  } else {
    task_queue_length_ += 1;
    task_execution_service_.post([=] {
      raylet_task_receiver_->HandleAssignTask(request, reply, send_reply_callback);
    });
  }
}

void CoreWorker::HandlePushTask(const rpc::PushTaskRequest &request,
                                rpc::PushTaskReply *reply,
                                rpc::SendReplyCallback send_reply_callback) {
  if (HandleWrongRecipient(WorkerID::FromBinary(request.intended_worker_id()),
                           send_reply_callback)) {
    return;
  }

  task_queue_length_ += 1;
  task_execution_service_.post([=] {
    direct_task_receiver_->HandlePushTask(request, reply, send_reply_callback);
  });
}

void CoreWorker::HandleDirectActorCallArgWaitComplete(
    const rpc::DirectActorCallArgWaitCompleteRequest &request,
    rpc::DirectActorCallArgWaitCompleteReply *reply,
    rpc::SendReplyCallback send_reply_callback) {
  if (HandleWrongRecipient(WorkerID::FromBinary(request.intended_worker_id()),
                           send_reply_callback)) {
    return;
  }

  task_execution_service_.post([=] {
    direct_task_receiver_->HandleDirectActorCallArgWaitComplete(request, reply,
                                                                send_reply_callback);
  });
}

void CoreWorker::HandleGetObjectStatus(const rpc::GetObjectStatusRequest &request,
                                       rpc::GetObjectStatusReply *reply,
                                       rpc::SendReplyCallback send_reply_callback) {
  ObjectID object_id = ObjectID::FromBinary(request.object_id());
  TaskID owner_id = TaskID::FromBinary(request.owner_id());
  if (owner_id != GetCallerId()) {
    RAY_LOG(INFO) << "Handling GetObjectStatus for object produced by previous task "
                  << owner_id.Hex();
  }
  // We own the task. Reply back to the borrower once the object has been
  // created.
  // TODO(swang): We could probably just send the object value if it is small
  // enough and we have it local.
  reply->set_status(rpc::GetObjectStatusReply::CREATED);
  if (task_manager_->IsTaskPending(object_id.TaskId())) {
    // Acquire a reference and retry. This prevents the object from being
    // evicted out from under us before we can start the get.
    AddLocalReference(object_id);
    if (task_manager_->IsTaskPending(object_id.TaskId())) {
      // The task is pending. Send the reply once the task finishes.
      memory_store_->GetAsync(object_id,
                              [send_reply_callback](std::shared_ptr<RayObject> obj) {
                                send_reply_callback(Status::OK(), nullptr, nullptr);
                              });
      RemoveLocalReference(object_id);
    } else {
      // We lost the race, the task is done.
      RemoveLocalReference(object_id);
      send_reply_callback(Status::OK(), nullptr, nullptr);
    }
  } else {
    // The task is done. Send the reply immediately.
    send_reply_callback(Status::OK(), nullptr, nullptr);
  }
}

void CoreWorker::HandleWaitForObjectEviction(
    const rpc::WaitForObjectEvictionRequest &request,
    rpc::WaitForObjectEvictionReply *reply, rpc::SendReplyCallback send_reply_callback) {
  if (HandleWrongRecipient(WorkerID::FromBinary(request.intended_worker_id()),
                           send_reply_callback)) {
    return;
  }

  // Send a response to trigger unpinning the object when it is no longer in scope.
  auto respond = [send_reply_callback](const ObjectID &object_id) {
    RAY_LOG(DEBUG) << "Replying to HandleWaitForObjectEviction for " << object_id;
    send_reply_callback(Status::OK(), nullptr, nullptr);
  };

  ObjectID object_id = ObjectID::FromBinary(request.object_id());
  // Returns true if the object was present and the callback was added. It might have
  // already been evicted by the time we get this request, in which case we should
  // respond immediately so the raylet unpins the object.
  if (!reference_counter_->SetDeleteCallback(object_id, respond)) {
    RAY_LOG(DEBUG) << "ObjectID reference already gone for " << object_id;
    respond(object_id);
  }
}

void CoreWorker::HandleKillActor(const rpc::KillActorRequest &request,
                                 rpc::KillActorReply *reply,
                                 rpc::SendReplyCallback send_reply_callback) {
  ActorID intended_actor_id = ActorID::FromBinary(request.intended_actor_id());
  if (intended_actor_id != worker_context_.GetCurrentActorID()) {
    std::ostringstream stream;
    stream << "Mismatched ActorID: ignoring KillActor for previous actor "
           << intended_actor_id
           << ", current actor ID: " << worker_context_.GetCurrentActorID();
    auto msg = stream.str();
    RAY_LOG(ERROR) << msg;
    send_reply_callback(Status::Invalid(msg), nullptr, nullptr);
    return;
  }
  RAY_LOG(INFO) << "Got KillActor, exiting immediately...";
  if (log_dir_ != "") {
    RayLog::ShutDownRayLog();
  }
  exit(1);
}

void CoreWorker::HandleGetCoreWorkerStats(const rpc::GetCoreWorkerStatsRequest &request,
                                          rpc::GetCoreWorkerStatsReply *reply,
                                          rpc::SendReplyCallback send_reply_callback) {
  absl::MutexLock lock(&mutex_);
  auto stats = reply->mutable_core_worker_stats();
  stats->set_num_pending_tasks(task_manager_->NumPendingTasks());
  stats->set_task_queue_length(task_queue_length_);
  stats->set_num_executed_tasks(num_executed_tasks_);
  stats->set_num_object_ids_in_scope(reference_counter_->NumObjectIDsInScope());
  if (!current_task_.TaskId().IsNil()) {
    stats->set_current_task_desc(current_task_.DebugString());
    for (auto const it : current_task_.FunctionDescriptor()) {
      stats->add_current_task_func_desc(it);
    }
  }
  stats->set_ip_address(rpc_address_.ip_address());
  stats->set_port(rpc_address_.port());
  stats->set_actor_id(actor_id_.Binary());
  auto used_resources_map = stats->mutable_used_resources();
  for (auto const &it : *resource_ids_) {
    double quantity = 0;
    for (auto const &pair : it.second) {
      quantity += pair.second;
    }
    (*used_resources_map)[it.first] = quantity;
  }
  stats->set_actor_title(actor_title_);
  google::protobuf::Map<std::string, std::string> webui_map(webui_display_.begin(),
                                                            webui_display_.end());
  (*stats->mutable_webui_display()) = webui_map;

  MemoryStoreStats memory_store_stats = memory_store_->GetMemoryStoreStatisticalData();
  stats->set_num_local_objects(memory_store_stats.num_local_objects);
  stats->set_used_object_store_memory(memory_store_stats.used_object_store_memory);
  send_reply_callback(Status::OK(), nullptr, nullptr);
}

void CoreWorker::YieldCurrentFiber(FiberEvent &event) {
  RAY_CHECK(worker_context_.CurrentActorIsAsync());
  boost::this_fiber::yield();
  event.Wait();
}

void CoreWorker::GetAsync(const ObjectID &object_id, SetResultCallback success_callback,
                          SetResultCallback fallback_callback, void *python_future) {
  RAY_CHECK(object_id.IsDirectCallType());
  memory_store_->GetAsync(object_id, [python_future, success_callback, fallback_callback,
                                      object_id](std::shared_ptr<RayObject> ray_object) {
    if (ray_object->IsInPlasmaError()) {
      fallback_callback(ray_object, object_id, python_future);
    } else {
      success_callback(ray_object, object_id, python_future);
    }
  });
}

void CoreWorker::SetActorId(const ActorID &actor_id) {
  absl::MutexLock lock(&mutex_);
  RAY_CHECK(actor_id_.IsNil());
  actor_id_ = actor_id;
}

void CoreWorker::SetWebuiDisplay(const std::string &key, const std::string &message) {
  absl::MutexLock lock(&mutex_);
  webui_display_[key] = message;
}

void CoreWorker::SetActorTitle(const std::string &title) {
  absl::MutexLock lock(&mutex_);
  actor_title_ = title;
}

}  // namespace ray
