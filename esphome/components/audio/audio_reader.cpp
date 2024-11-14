#ifdef USE_ESP32

#include "audio_reader.h"

#include "esphome/core/defines.h"
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

esp_err_t AudioReader::add_sink(std::weak_ptr<RingBuffer> output_ring_buffer) {
  if (current_audio_file_ != nullptr) {
    // A transfer buffer isn't ncessary for a local file
    this->file_ring_buffer_ = output_ring_buffer.lock();
    return ESP_OK;
  }

  if (this->output_transfer_buffer_ != nullptr) {
    this->output_transfer_buffer_->set_sink(output_ring_buffer);
    return ESP_OK;
  }

  return ESP_ERR_INVALID_STATE;
}

esp_err_t AudioReader::start(AudioFile *audio_file, AudioFileType &file_type) {
  file_type = AudioFileType::NONE;

  this->current_audio_file_ = audio_file;

  this->file_current_ = audio_file->data;
  file_type = audio_file->file_type;

  return ESP_OK;
}

esp_err_t AudioReader::start(const std::string &uri, AudioFileType &file_type) {
  file_type = AudioFileType::NONE;

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

  esp_err_t err = ESP_OK;

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
  }
#ifdef USE_AUDIO_MP3_SUPPORT
  else if (str_endswith(url_string, ".mp3")) {
    file_type = AudioFileType::MP3;
  }
#endif
#ifdef USE_AUDIO_FLAC_SUPPORT
  else if (str_endswith(url_string, ".flac")) {
    file_type = AudioFileType::FLAC;
  }
#endif
  else {
    file_type = AudioFileType::NONE;
    this->cleanup_connection_();
    return ESP_ERR_NOT_SUPPORTED;
  }

  this->no_data_read_count_ = 0;

  this->output_transfer_buffer_ = AudioSinkTransferBuffer::create(this->buffer_size_);
  if (this->output_transfer_buffer_ == nullptr) {
    return ESP_ERR_NO_MEM;
  }

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
  size_t remaining_bytes = this->current_audio_file_->length - (this->file_current_ - this->current_audio_file_->data);
  if (remaining_bytes > 0) {
    size_t bytes_written = this->file_ring_buffer_->write_without_replacement(this->file_current_, remaining_bytes,
                                                                              pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
    this->file_current_ += bytes_written;

    return AudioReaderState::READING;
  }

  return AudioReaderState::FINISHED;
}

AudioReaderState AudioReader::http_read_() {
  this->output_transfer_buffer_->transfer_data_to_sink(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));

  if (esp_http_client_is_complete_data_received(this->client_)) {
    if (this->output_transfer_buffer_->available() == 0) {
      this->cleanup_connection_();
      return AudioReaderState::FINISHED;
    }
  } else {
    size_t bytes_to_read = this->output_transfer_buffer_->free();
    int received_len =
        esp_http_client_read(this->client_, (char *) this->output_transfer_buffer_->get_buffer_end(), bytes_to_read);

    if (received_len > 0) {
      this->output_transfer_buffer_->increase_buffer_length(received_len);

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
