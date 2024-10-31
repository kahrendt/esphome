#ifdef USE_ESP_IDF

#include "audio_reader.h"

#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

namespace esphome {
namespace audio {

static const size_t READ_WRITE_TIMEOUT_MS = 20;

// The number of times the http read times out with no data before throwing an error
static const ssize_t ERROR_COUNT_NO_DATA_READ_TIMEOUT = 100;

static const size_t HTTP_STREAM_BUFFER_SIZE = 2048;

AudioReader::~AudioReader() { this->cleanup_connection_(); }

esp_err_t AudioReader::start(AudioFile *audio_file, AudioFileType &file_type) {
  file_type = AudioFileType::NONE;

  esp_err_t err = this->allocate_output_buffer_();
  if (err != ESP_OK) {
    return err;
  }

  this->current_audio_file_ = audio_file;

  this->output_buffer_current_ = audio_file->data;
  this->output_buffer_length_ = audio_file->length;
  file_type = audio_file->file_type;

  return ESP_OK;
}

esp_err_t AudioReader::start(const std::string &uri, AudioFileType &file_type) {
  file_type = AudioFileType::NONE;

  esp_err_t err = this->allocate_output_buffer_();
  if (err != ESP_OK) {
    return err;
  }

  this->cleanup_connection_();

  if (uri.empty()) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_http_client_config_t client_config = {};

  client_config.url = uri.c_str();
  client_config.cert_pem = nullptr;
  client_config.disable_auto_redirect = false;
  client_config.max_redirection_count = 10;
  client_config.buffer_size = HTTP_STREAM_BUFFER_SIZE;
  client_config.keep_alive_enable = true;
  client_config.timeout_ms = 5000;  // Doesn't raise an error if exceeded in esp-idf v4.4, it just prevents the
                                    // http_client_read command from blocking for too long

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (uri.find("https:") != std::string::npos) {
    client_config.crt_bundle_attach = esp_crt_bundle_attach;
  }
#endif

  this->client_ = esp_http_client_init(&client_config);

  if (this->client_ == nullptr) {
    return ESP_FAIL;
  }

  if ((err = esp_http_client_open(this->client_, 0)) != ESP_OK) {
    this->cleanup_connection_();
    return err;
  }

  esp_http_client_fetch_headers(this->client_);

  char url[500];
  err = esp_http_client_get_url(this->client_, url, 500);
  if (err != ESP_OK) {
    this->cleanup_connection_();
    return err;
  }

  std::string url_string = url;

  if (str_endswith(url_string, ".wav")) {
    file_type = AudioFileType::WAV;
  } else if (str_endswith(url_string, ".mp3")) {
    file_type = AudioFileType::MP3;
  } else if (str_endswith(url_string, ".flac")) {
    file_type = AudioFileType::FLAC;
  } else {
    file_type = AudioFileType::NONE;
    this->cleanup_connection_();
    return ESP_ERR_NOT_SUPPORTED;
  }

  this->output_buffer_current_ = this->output_buffer_;
  this->output_buffer_length_ = 0;
  this->no_data_read_count_ = 0;

  return ESP_OK;
}

AudioReaderState AudioReader::read() {
  if (this->client_ != nullptr) {
    return this->http_read_();
  } else if (this->current_audio_file_ != nullptr) {
    return this->file_read_();
  }

  return AudioReaderState::FAILED;
}

AudioReaderState AudioReader::file_read_() {
  if (this->output_buffer_length_ > 0) {
    size_t bytes_written = this->output_ring_buffer_->write_without_replacement(
        (void *) this->output_buffer_current_, this->output_buffer_length_, pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
    this->output_buffer_length_ -= bytes_written;
    this->output_buffer_current_ += bytes_written;

    return AudioReaderState::READING;
  }
  return AudioReaderState::FINISHED;
}

AudioReaderState AudioReader::http_read_() {
  this->write_ring_buffer_(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));

  if (esp_http_client_is_complete_data_received(this->client_)) {
    if (this->output_buffer_length_ == 0) {
      this->cleanup_connection_();
      printf("reader - ouput ring buffer owners: %ld\n", this->output_ring_buffer_.use_count());
      vTaskDelay(pdMS_TO_TICKS(50));
      return AudioReaderState::FINISHED;
    }
  } else {
    size_t bytes_to_read = this->output_buffer_size_ - this->output_buffer_length_;
    int received_len =
        esp_http_client_read(this->client_, (char *) this->output_buffer_ + this->output_buffer_length_, bytes_to_read);

    if (received_len > 0) {
      this->output_buffer_length_ += received_len;
      this->no_data_read_count_ = 0;
    } else if (received_len < 0) {
      // HTTP read error
      this->cleanup_connection_();
      return AudioReaderState::FAILED;
    } else {
      if (bytes_to_read > 0) {
        // Read timed out
        ++this->no_data_read_count_;
        if (this->no_data_read_count_ >= ERROR_COUNT_NO_DATA_READ_TIMEOUT) {
          // Timed out with no data read too many times, so the http read has failed
          this->cleanup_connection_();
          return AudioReaderState::FAILED;
        }
        vTaskDelay(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
      }
    }
  }

  return AudioReaderState::READING;
}

void AudioReader::cleanup_connection_() {
  if (this->client_ != nullptr) {
    esp_http_client_close(this->client_);
    esp_http_client_cleanup(this->client_);
    this->client_ = nullptr;
  }
}

}  // namespace audio
}  // namespace esphome

#endif
