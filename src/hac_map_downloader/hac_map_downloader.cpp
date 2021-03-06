#include <cstdio>
#include <cstring>
#include <thread>

#define CURL_STATICLIB
#include <curl/curl.h>

#include "hac_map_downloader.hpp"

void HACMapDownloader::dispatch_thread(HACMapDownloader *downloader) {
    // Set the URL
    CURLcode result;
    unsigned int repo = 1;
    do {
        char url[255];
        std::snprintf(url, sizeof(url), "http://maps%u.halonet.net/halonet/locator.php?format=7z&map=%s", repo, downloader->map.data());
        curl_easy_setopt(downloader->curl, CURLOPT_URL, url);
        downloader->download_started = Clock::now();
        result = curl_easy_perform(downloader->curl);
        repo++;
    }
    while(result != CURLcode::CURLE_COULDNT_RESOLVE_HOST && result != CURLcode::CURLE_OK);

    // Note that we're extracting; clean up CURL
    downloader->mutex.lock();
    curl_easy_cleanup(downloader->curl);
    downloader->curl = nullptr;

    // Write the last data
    std::fwrite(downloader->buffer.data(), downloader->buffer_used, 1, downloader->output_file_handle);
    downloader->buffer_used = 0;
    downloader->buffer.clear();

    // Close the file handle
    std::fclose(downloader->output_file_handle);

    downloader->status = DOWNLOAD_STAGE_COMPLETE;
    downloader->mutex.unlock();
}

std::size_t HACMapDownloader::get_download_speed() noexcept {
    // If we haven't started, return 0
    if(this->downloaded_size == 0) {
        return 0.0;
    }

    auto now = Clock::now();
    auto difference = now - this->download_started;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(difference).count();

    // Don't divide by zero
    if(ms <= 0) {
        return 0.0;
    }

    return (this->downloaded_size) / ms;
}

// Callback class
class HACMapDownloader::HACMapDownloaderCallback {
public:
    // When we've received data, put it in here
    static size_t write_callback(const std::byte *ptr, std::size_t, std::size_t nmemb, HACMapDownloader *userdata) {
        userdata->mutex.lock();
        userdata->status = HACMapDownloader::DOWNLOAD_STAGE_DOWNLOADING;
        if(userdata->buffer_used + nmemb > userdata->buffer.size()) {
            std::fwrite(userdata->buffer.data(), userdata->buffer_used, 1, userdata->output_file_handle);
            std::fwrite(ptr, nmemb, 1, userdata->output_file_handle);
            userdata->buffer_used = 0;
        }
        else {
            std::copy(ptr, ptr + nmemb, userdata->buffer.data() + userdata->buffer_used);
            userdata->buffer_used += nmemb;
        }

        userdata->mutex.unlock();
        return nmemb;
    }

    // When progress has been made, record it here
    static int progress_callback(HACMapDownloader *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
        clientp->mutex.lock();
        clientp->downloaded_size = dlnow;
        clientp->total_size = dltotal;
        clientp->mutex.unlock();
        return 0;
    }
};

// Set up stuff
void HACMapDownloader::dispatch() {
    // Lock the mutex
    this->mutex.lock();
    if(this->curl) {
        this->mutex.unlock();
        std::terminate();
        return;
    }

    #ifndef TEST_EXTRACTION_ONLY
    this->output_file_handle = std::fopen(this->output_file.data(), "wb");

    // If we failed to open, give up and close, unlocking the mutex
    if(!this->output_file_handle) {
        this->status = HACMapDownloader::DOWNLOAD_STAGE_FAILED;
        this->mutex.unlock();
        return;
    }

    // Initialize cURL as well as the downloader variables
    this->curl = curl_easy_init();

    // Set our callbacks
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, HACMapDownloaderCallback::progress_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, HACMapDownloaderCallback::write_callback);

    // Set the data passed to the callbacks so our class can be updated
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, this);

    // Enable progress bar
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);

    // Fail on error
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);

    // 10 second timeout
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    #endif

    // Set the download stage to starting
    this->status = HACMapDownloader::DOWNLOAD_STAGE_STARTING;

    // Hold 1 MiB of data in memory
    this->buffer.insert(this->buffer.end(), 1024 * 1024, std::byte());
    this->buffer_used = 0;

    // Set the number of bytes downloaded to 0
    this->downloaded_size = 0;
    this->total_size = 0;

    // Unlock
    this->mutex.unlock();

    std::thread(HACMapDownloader::dispatch_thread, this).detach();
}

HACMapDownloader::DownloadStage HACMapDownloader::get_status() noexcept {
    this->mutex.lock();
    auto return_value = this->status;
    this->mutex.unlock();
    return return_value;
}

std::size_t HACMapDownloader::get_downloaded_size() noexcept {
    this->mutex.lock();
    std::size_t return_value = this->downloaded_size;
    this->mutex.unlock();
    return return_value;
}

std::size_t HACMapDownloader::get_total_size() noexcept {
    this->mutex.lock();
    std::size_t return_value = this->total_size;
    this->mutex.unlock();
    return return_value;
}

bool HACMapDownloader::is_finished() noexcept {
    auto m = this->mutex.try_lock();
    if(!m) {
        return false;
    }
    bool finished = this->status == DOWNLOAD_STAGE_COMPLETE || this->status == DOWNLOAD_STAGE_FAILED || this->status == DOWNLOAD_STAGE_NOT_STARTED;
    this->mutex.unlock();
    return finished;
}

HACMapDownloader::HACMapDownloader(const char *map, const char *output_file) : map(map), output_file(output_file) {}
HACMapDownloader::~HACMapDownloader() {
    while(!this->is_finished());
}
