/*
 * Telegram REAPER extension.
 *
 * Starts Telegram multipart uploads on worker threads so the REAPER main
 * thread never waits for a large file or a slow network connection.
 */

#include "reaper_plugin_min.h"

#include <curl/curl.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int kPending = 0;
constexpr int kSuccess = 1;
constexpr int kFailure = -1;

struct Job {
  int id = 0;
  std::string token;
  std::string chat_id;
  std::string file_path;
  std::string caption;
  std::atomic<int> state{kPending};
  std::atomic_bool cancel{false};
  std::mutex mutex;
  std::string error;
  std::string response;
};

std::mutex g_jobs_mutex;
std::unordered_map<int, std::shared_ptr<Job>> g_jobs;
std::mutex g_queue_mutex;
std::condition_variable g_queue_wake;
std::queue<std::shared_ptr<Job>> g_queue;
std::vector<std::thread> g_workers;
std::atomic_bool g_stopping{false};
std::atomic_int g_next_id{1};
reaper_plugin_info_t* g_reaper = nullptr;
thread_local double g_vararg_result = 0.0;
thread_local std::string g_vararg_text;

void SetError(const std::shared_ptr<Job>& job, std::string error)
{
  std::lock_guard<std::mutex> lock(job->mutex);
  job->error = std::move(error);
}

bool IsCancelled(const std::shared_ptr<Job>& job)
{
  return g_stopping.load() || job->cancel.load();
}

int ProgressCallback(void* userdata, curl_off_t, curl_off_t,
                    curl_off_t, curl_off_t)
{
  const auto* job = static_cast<Job*>(userdata);
  return (g_stopping.load() || job->cancel.load()) ? 1 : 0;
}

size_t ResponseCallback(char* data, size_t size, size_t count, void* userdata)
{
  auto* response = static_cast<std::string*>(userdata);
  response->append(data, size * count);
  return size * count;
}

std::shared_ptr<Job> FindJob(int id)
{
  std::lock_guard<std::mutex> lock(g_jobs_mutex);
  const auto it = g_jobs.find(id);
  return it == g_jobs.end() ? nullptr : it->second;
}

void RunJob(const std::shared_ptr<Job>& job)
{
  if (IsCancelled(job)) {
    SetError(job, "cancelled");
    job->state = kFailure;
    return;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    SetError(job, "curl_easy_init failed");
    job->state = kFailure;
    return;
  }

  curl_mime* mime = curl_mime_init(curl);
  curl_mimepart* part = nullptr;
  CURLcode result = CURLE_FAILED_INIT;

  if (!mime) {
    SetError(job, "curl_mime_init failed");
  } else if ((part = curl_mime_addpart(mime)) == nullptr ||
             curl_mime_name(part, "chat_id") != CURLE_OK ||
             curl_mime_data(part, job->chat_id.c_str(), CURL_ZERO_TERMINATED) != CURLE_OK) {
    SetError(job, "could not prepare chat_id");
  } else if ((part = curl_mime_addpart(mime)) == nullptr ||
             curl_mime_name(part, "document") != CURLE_OK ||
             curl_mime_filedata(part, job->file_path.c_str()) != CURLE_OK) {
    SetError(job, "could not open upload file");
  } else if (!job->caption.empty() &&
             ((part = curl_mime_addpart(mime)) == nullptr ||
              curl_mime_name(part, "caption") != CURLE_OK ||
              curl_mime_data(part, job->caption.c_str(), CURL_ZERO_TERMINATED) != CURLE_OK)) {
    SetError(job, "could not prepare caption");
  } else {
    const std::string url = "https://api.telegram.org/bot" + job->token + "/sendDocument";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ResponseCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &job->response);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, job.get());
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "REAPER-Telegram-Uploader/1.0");
    result = curl_easy_perform(curl);
  }

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  if (IsCancelled(job)) {
    SetError(job, "cancelled");
    job->state = kFailure;
  } else if (result != CURLE_OK) {
    SetError(job, curl_easy_strerror(result));
    job->state = kFailure;
  } else if (http_code < 200 || http_code >= 300 ||
             job->response.find("\"ok\":true") == std::string::npos) {
    SetError(job, "Telegram rejected the upload (HTTP " + std::to_string(http_code) + ")");
    job->state = kFailure;
  } else {
    job->state = kSuccess;
  }

  if (mime) curl_mime_free(mime);
  curl_easy_cleanup(curl);
}

void WorkerLoop()
{
  for (;;) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock<std::mutex> lock(g_queue_mutex);
      g_queue_wake.wait(lock, [] {
        return g_stopping.load() || !g_queue.empty();
      });
      if (g_stopping.load() && g_queue.empty()) return;
      job = std::move(g_queue.front());
      g_queue.pop();
    }
    RunJob(job);
  }
}

void StartWorkers()
{
  g_stopping = false;
  g_workers.emplace_back(WorkerLoop);
}

void StopWorkers()
{
  g_stopping = true;
  {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    for (auto& entry : g_jobs) entry.second->cancel = true;
  }
  g_queue_wake.notify_all();
  for (auto& worker : g_workers) {
    if (worker.joinable()) worker.join();
  }
  g_workers.clear();
}

double Telegram_StartUpload(char* token, char* chat_id, char* file_path, char* caption)
{
  if (!token || !chat_id || !file_path || !*token || !*chat_id || !*file_path || g_stopping.load())
    return -1.0;

  auto job = std::make_shared<Job>();
  job->id = g_next_id.fetch_add(1);
  job->token = token;
  job->chat_id = chat_id;
  job->file_path = file_path;
  if (caption) job->caption = caption;

  {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    g_jobs.emplace(job->id, job);
  }
  {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_queue.push(job);
  }
  g_queue_wake.notify_one();
  return static_cast<double>(job->id);
}

double Telegram_PollUpload(double id)
{
  const auto job = FindJob(static_cast<int>(id));
  return job ? static_cast<double>(job->state.load()) : -1.0;
}

char* Telegram_UploadError(double id)
{
  const auto job = FindJob(static_cast<int>(id));
  if (!job) {
    g_vararg_text = "unknown Telegram upload job";
    return g_vararg_text.data();
  }
  std::lock_guard<std::mutex> lock(job->mutex);
  g_vararg_text = job->error;
  return g_vararg_text.data();
}

char* Telegram_UploadResponse(double id)
{
  const auto job = FindJob(static_cast<int>(id));
  if (!job) {
    g_vararg_text = "";
    return g_vararg_text.data();
  }
  std::lock_guard<std::mutex> lock(job->mutex);
  g_vararg_text = job->response;
  return g_vararg_text.data();
}

double Telegram_CancelUpload(double id)
{
  const auto job = FindJob(static_cast<int>(id));
  if (!job) return 0.0;
  job->cancel = true;
  return 1.0;
}

double Telegram_ReleaseUpload(double id)
{
  std::lock_guard<std::mutex> lock(g_jobs_mutex);
  return g_jobs.erase(static_cast<int>(id)) ? 1.0 : 0.0;
}

const char kDefStart[] =
  "double\0char*,char*,char*,char*\0token,chat_id,file_path,caption\0"
  "Start a non-blocking Telegram document upload\0";
const char kDefPoll[] =
  "double\0double\0job\0Return 0 while pending, 1 on success, -1 on failure\0";
const char kDefError[] =
  "char*\0double\0job\0Return the Telegram upload error\0";
const char kDefResponse[] =
  "char*\0double\0job\0Return the Telegram API response\0";
const char kDefCancel[] =
  "double\0double\0job\0Cancel a Telegram upload\0";
const char kDefRelease[] =
  "double\0double\0job\0Release a completed Telegram upload job\0";

void* VarargStart(void** args, int count)
{
  g_vararg_result = (args && count >= 4)
    ? Telegram_StartUpload(static_cast<char*>(args[0]), static_cast<char*>(args[1]),
                          static_cast<char*>(args[2]), static_cast<char*>(args[3]))
    : -1.0;
  return &g_vararg_result;
}

void* VarargPoll(void** args, int count)
{
  g_vararg_result = (args && count >= 1 && args[0])
    ? Telegram_PollUpload(*static_cast<double*>(args[0])) : -1.0;
  return &g_vararg_result;
}

void* VarargError(void** args, int count)
{
  return (args && count >= 1 && args[0])
    ? Telegram_UploadError(*static_cast<double*>(args[0]))
    : const_cast<char*>("invalid Telegram upload job");
}

void* VarargResponse(void** args, int count)
{
  return (args && count >= 1 && args[0])
    ? Telegram_UploadResponse(*static_cast<double*>(args[0])) : const_cast<char*>("");
}

void* VarargCancel(void** args, int count)
{
  g_vararg_result = (args && count >= 1 && args[0])
    ? Telegram_CancelUpload(*static_cast<double*>(args[0])) : 0.0;
  return &g_vararg_result;
}

void* VarargRelease(void** args, int count)
{
  g_vararg_result = (args && count >= 1 && args[0])
    ? Telegram_ReleaseUpload(*static_cast<double*>(args[0])) : 0.0;
  return &g_vararg_result;
}

void RegisterApi(reaper_plugin_info_t* rec, const char* name, void* func,
                 void* vararg, const char* def)
{
  rec->Register((std::string("API_") + name).c_str(), func);
  rec->Register((std::string("APIvararg_") + name).c_str(), vararg);
  rec->Register((std::string("APIdef_") + name).c_str(), const_cast<char*>(def));
}

void UnregisterApi(reaper_plugin_info_t* rec, const char* name, void* func,
                   void* vararg)
{
  rec->Register((std::string("-API_") + name).c_str(), func);
  rec->Register((std::string("-APIvararg_") + name).c_str(), vararg);
  rec->Register((std::string("-APIdef_") + name).c_str(), nullptr);
}

} // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* rec)
{
  if (!rec) {
    if (g_reaper) {
      UnregisterApi(g_reaper, "Telegram_StartUpload",
                    reinterpret_cast<void*>(Telegram_StartUpload),
                    reinterpret_cast<void*>(VarargStart));
      UnregisterApi(g_reaper, "Telegram_PollUpload",
                    reinterpret_cast<void*>(Telegram_PollUpload),
                    reinterpret_cast<void*>(VarargPoll));
      UnregisterApi(g_reaper, "Telegram_UploadError",
                    reinterpret_cast<void*>(Telegram_UploadError),
                    reinterpret_cast<void*>(VarargError));
      UnregisterApi(g_reaper, "Telegram_UploadResponse",
                    reinterpret_cast<void*>(Telegram_UploadResponse),
                    reinterpret_cast<void*>(VarargResponse));
      UnregisterApi(g_reaper, "Telegram_CancelUpload",
                    reinterpret_cast<void*>(Telegram_CancelUpload),
                    reinterpret_cast<void*>(VarargCancel));
      UnregisterApi(g_reaper, "Telegram_ReleaseUpload",
                    reinterpret_cast<void*>(Telegram_ReleaseUpload),
                    reinterpret_cast<void*>(VarargRelease));
      StopWorkers();
      curl_global_cleanup();
      g_reaper = nullptr;
    }
    return 0;
  }
  if (rec->caller_version < REAPER_PLUGIN_VERSION) return 0;

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 0;
  g_reaper = rec;
  StartWorkers();

  RegisterApi(rec, "Telegram_StartUpload", reinterpret_cast<void*>(Telegram_StartUpload),
              reinterpret_cast<void*>(VarargStart), kDefStart);
  RegisterApi(rec, "Telegram_PollUpload", reinterpret_cast<void*>(Telegram_PollUpload),
              reinterpret_cast<void*>(VarargPoll), kDefPoll);
  RegisterApi(rec, "Telegram_UploadError", reinterpret_cast<void*>(Telegram_UploadError),
              reinterpret_cast<void*>(VarargError), kDefError);
  RegisterApi(rec, "Telegram_UploadResponse", reinterpret_cast<void*>(Telegram_UploadResponse),
              reinterpret_cast<void*>(VarargResponse), kDefResponse);
  RegisterApi(rec, "Telegram_CancelUpload", reinterpret_cast<void*>(Telegram_CancelUpload),
              reinterpret_cast<void*>(VarargCancel), kDefCancel);
  RegisterApi(rec, "Telegram_ReleaseUpload", reinterpret_cast<void*>(Telegram_ReleaseUpload),
              reinterpret_cast<void*>(VarargRelease), kDefRelease);
  return 1;
}
