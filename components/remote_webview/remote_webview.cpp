#include "remote_webview.h"
#include "remote_webview_config.h"
#include "esphome/core/log.h"

#include "esp_idf_version.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "esp_efuse.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace remote_webview {

static const char *const TAG = "Remote_WebView";
RemoteWebView *RemoteWebView::self_ = nullptr;

//Below two functions are part of automation template testing
void RemoteWebView::add_on_frame_update_callback(std::function<void()> &&callback) {
  this->on_frame_update_callback_.add(std::move(callback));
}
void RemoteWebView::trigger_on_frame_update() {
  uint32_t now = millis();
  
  if (now - this->last_trigger_ms_ < 1000) return;
  
  this->last_trigger_ms_ = now;
  
  ESP_LOGD(TAG, "Triggering the on_frame_update automation");
  this->on_frame_update_callback_.call();
}

void RemoteWebView::process_current_url_packet_(const uint8_t *data, size_t len) {
  if (!data || len < sizeof(proto::CurrentURLHeader)) return;
  
  auto *hdr = reinterpret_cast<const proto::CurrentURLHeader *>(data);
  if (sizeof(proto::CurrentURLHeader) + hdr->url_len > len) return;
  
  if (this->url_sensor_ == nullptr) return;

  std::string url(reinterpret_cast<const char*>(data + sizeof(proto::CurrentURLHeader)), hdr->url_len);
  if (this->state_mtx_ && xSemaphoreTake(this->state_mtx_, pdMS_TO_TICKS(10)) == pdTRUE) {
    this->pending_url_ = std::move(url);
    this->url_publish_pending_.store(true, std::memory_order_release);
    xSemaphoreGive(this->state_mtx_);
  }
}

std::string RemoteWebView::get_current_url() const {
  if (this->url_sensor_ != nullptr && this->url_sensor_->has_state()) {
    return this->url_sensor_->state;
  }
  return "";
}

// True when both messages are Frame packets carrying the same frame id —
// evicting one of those would tear a frame the delta protocol never repairs.
bool RemoteWebView::is_same_frame_(const WsMsg &a, const WsMsg &b) {
  if (a.len < sizeof(proto::FrameHeader) || b.len < sizeof(proto::FrameHeader)) return false;
  if (a.buf[0] != (uint8_t) proto::MsgType::Frame || b.buf[0] != (uint8_t) proto::MsgType::Frame) return false;
  return proto::rd32(a.buf + 2) == proto::rd32(b.buf + 2);
}

static inline void websocket_force_reconnect(esp_websocket_client_handle_t client) {
  if (!client) return;
  esp_websocket_client_stop(client);
  esp_websocket_client_start(client);
}

void RemoteWebView::setup() {
  self_ = this;

  if (!display_) {
    ESP_LOGE(TAG, "no display");
    return;
  }

  display_width_ = display_->get_width();
  display_height_ = display_->get_height();

  q_decode_ = xQueueCreate(cfg::decode_queue_depth, sizeof(WsMsg));
  q_send_ = xQueueCreate(cfg::send_queue_depth, sizeof(OutMsg));
  ws_send_mtx_ = xSemaphoreCreateMutex();
  state_mtx_ = xSemaphoreCreateMutex();

  reasm_buf_size_ = (max_bytes_per_msg_ > 0) ? (size_t) max_bytes_per_msg_ : cfg::ws_max_message_bytes;
  const int pool_size = cfg::decode_queue_depth + cfg::msg_pool_extra;
  q_free_ = xQueueCreate(pool_size, sizeof(uint8_t *));
  int allocated = 0;
  size_t internal_bytes = 0;
  for (int i = 0; i < pool_size; i++) {
    auto *b = (uint8_t *) heap_caps_malloc(reasm_buf_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) {
      if (internal_bytes + reasm_buf_size_ > cfg::msg_pool_max_internal_bytes) break;
      b = (uint8_t *) heap_caps_malloc(reasm_buf_size_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (!b) break;
      internal_bytes += reasm_buf_size_;
    }
    xQueueSend(q_free_, &b, 0);
    allocated++;
  }
  if (allocated < pool_size)
    // Fewer than (queue depth + 2) buffers means multi-message frames will
    // drop packets and leave stale tiles — this is a degraded mode, not a
    // cosmetic warning.
    ESP_LOGW(TAG,
             "allocated %d/%d message buffers (%u bytes each) — expect dropped "
             "frames; add PSRAM or lower max_bytes_per_msg",
             allocated, pool_size, (unsigned) reasm_buf_size_);
  if (allocated == 0)
    ESP_LOGE(TAG, "no message buffers available, streaming disabled (no PSRAM? lower max_bytes_per_msg)");

  start_decode_task_();
  start_ws_task_();

  if (touch_) {
    touch_listener_ = new RemoteWebViewTouchListener(this);
    touch_->register_listener(touch_listener_);
    ESP_LOGD(TAG, "touch listener registered");
  }

#if REMOTE_WEBVIEW_HW_JPEG
  jpeg_decode_engine_cfg_t jcfg = {
    .timeout_ms = 200,
  };
  if (jpeg_new_decoder_engine(&jcfg, &hw_dec_) != ESP_OK) {
    hw_dec_ = nullptr;
  }
  
  if (hw_dec_) {
    const int W = display_->get_width();
    const int H = display_->get_height();
    const int aligned_w = (W + 15) & ~15;
    const int aligned_h = (H + 15) & ~15;
    
    const size_t max_buffer_size = (size_t)aligned_w * (size_t)aligned_h * 2u;

    jpeg_decode_memory_alloc_cfg_t in_cfg { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t out_cfg { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };

    // Input holds compressed data only, bounded by the WS message size —
    // sizing it like the RGB565 output wasted ~20x the needed DMA memory.
    hw_decode_input_buf_ = (uint8_t*)jpeg_alloc_decoder_mem((uint32_t)reasm_buf_size_, &in_cfg, &hw_decode_input_size_);
    hw_decode_output_buf_ = (uint8_t*)jpeg_alloc_decoder_mem((uint32_t)max_buffer_size, &out_cfg, &hw_decode_output_size_);
    
    if (!hw_decode_input_buf_ || !hw_decode_output_buf_) {
      ESP_LOGE(TAG, "Failed to allocate HW decoder buffers");
      if (hw_decode_input_buf_) free(hw_decode_input_buf_);
      if (hw_decode_output_buf_) free(hw_decode_output_buf_);
      hw_decode_input_buf_ = nullptr;
      hw_decode_output_buf_ = nullptr;
      jpeg_del_decoder_engine(hw_dec_);
      hw_dec_ = nullptr;
    } else {
      ESP_LOGD(TAG, "HW decoder buffers allocated: input=%u, output=%u", 
               (unsigned)hw_decode_input_size_, (unsigned)hw_decode_output_size_);
    }
  }
#endif
}

void RemoteWebView::loop() {
  if (this->frame_update_pending_.exchange(false, std::memory_order_acq_rel)) {
    this->trigger_on_frame_update();
  }

  if (!this->url_sensor_) return;
  if (!this->url_publish_pending_.exchange(false, std::memory_order_acq_rel)) return;

  std::string url;
  if (this->state_mtx_ && xSemaphoreTake(this->state_mtx_, pdMS_TO_TICKS(10)) == pdTRUE) {
    url = this->pending_url_;
    xSemaphoreGive(this->state_mtx_);
  }

  if (!url.empty()) {
    this->url_sensor_->publish_state(url);
    ESP_LOGD(TAG, "Current Server URL updated: %s", url.c_str());
  }
}

void RemoteWebView::dump_config() {
  ESP_LOGCONFIG(TAG, "remote_webview:");

  const std::string id = device_id_.empty() ? resolve_device_id_() : device_id_;
  ESP_LOGCONFIG(TAG, "  id: %s", id.c_str());

  if (display_) {
    ESP_LOGCONFIG(TAG, "  display: %dx%d", display_->get_width(), display_->get_height());
  }

#if REMOTE_WEBVIEW_HW_JPEG
  ESP_LOGCONFIG(TAG, "  hw_jpeg: %s", hw_dec_ ? "yes" : "no");
#else
  ESP_LOGCONFIG(TAG, "  hw_jpeg: no");
#endif

  ESP_LOGCONFIG(TAG, "  server: %s:%d", server_host_.c_str(), server_port_);
  ESP_LOGCONFIG(TAG, "  url: %s", url_.c_str());

  auto print_opt_int = [&](const char *name, int v) {
    if (v >= 0) ESP_LOGCONFIG(TAG, "  %s: %d", name, v);
  };
  auto print_opt_float2 = [&](const char *name, float v) {
    if (v >= 0.0f) ESP_LOGCONFIG(TAG, "  %s: %.2f", name, (double)v);
  };

  print_opt_int   ("tile_size",                 tile_size_);
  print_opt_int   ("full_frame_tile_count",     full_frame_tile_count_);
  print_opt_float2("full_frame_area_threshold", full_frame_area_threshold_);
  print_opt_int   ("full_frame_every",          full_frame_every_);
  print_opt_int   ("every_nth_frame",           every_nth_frame_);
  print_opt_int   ("min_frame_interval",        min_frame_interval_);
  print_opt_int   ("jpeg_quality",              jpeg_quality_);
  print_opt_int   ("max_bytes_per_msg",         max_bytes_per_msg_);
  print_opt_int   ("big_endian",                rgb565_big_endian_);
  print_opt_int   ("rotation",                  rotation_);
}

bool RemoteWebView::open_url(const std::string &s) {
  if (s.empty()) return false;
  
  if (!ws_client_ || !esp_websocket_client_is_connected(ws_client_))
    return false;
  
  if (ws_send_open_url_(s.c_str(), 0)) {
    url_ = s;
    ESP_LOGD(TAG, "opened URL: %s", s.c_str());
    return true;
  }
  
  return false;
}

void RemoteWebView::start_ws_task_() {
  xTaskCreatePinnedToCore(&RemoteWebView::ws_task_tramp_, "rwv_ws", cfg::ws_task_stack, this, 5, &t_ws_, 0);
}

void RemoteWebView::ws_task_tramp_(void *arg) {
  auto *self = reinterpret_cast<RemoteWebView*>(arg);

  std::string uri_str = self->build_ws_uri_();
  esp_websocket_client_config_t cfg_ws = {};
  cfg_ws.uri = uri_str.c_str();
  cfg_ws.reconnect_timeout_ms = 2000;
  cfg_ws.network_timeout_ms   = 10000;
  cfg_ws.task_stack           = cfg::ws_task_stack;
  cfg_ws.task_prio            = cfg::ws_task_prio;
  cfg_ws.buffer_size          = cfg::ws_buffer_size;
  cfg_ws.disable_auto_reconnect = false;

  WsReasm reasm{};
  esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg_ws);
  ESP_ERROR_CHECK(esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler_, &reasm));
  ESP_ERROR_CHECK(esp_websocket_client_start(client));

  uint64_t last_supervise_us = esp_timer_get_time();
  uint64_t last_up_us = esp_timer_get_time();
  uint64_t last_forced_us = 0;
  for (;;) {
    OutMsg m;
    if (xQueueReceive(self->q_send_, &m, pdMS_TO_TICKS(250)) == pdTRUE) {
      do {
        const uint8_t *payload = m.ext ? m.ext : m.buf;
        const size_t payload_len = m.ext ? m.ext_len : m.len;
        if (self->ws_client_ && self->ws_send_mtx_ && esp_websocket_client_is_connected(self->ws_client_)) {
          // Large payloads (OpenURL) are rare; give them the longer timeout
          // the old synchronous path had.
          const TickType_t send_to = m.ext ? pdMS_TO_TICKS(200) : pdMS_TO_TICKS(50);
          if (xSemaphoreTake(self->ws_send_mtx_, pdMS_TO_TICKS(50)) == pdTRUE) {
            esp_websocket_client_send_bin(self->ws_client_, (const char *) payload, (int) payload_len, send_to);
            xSemaphoreGive(self->ws_send_mtx_);
          }
        }
        if (m.ext) free(m.ext);
      } while (xQueueReceive(self->q_send_, &m, 0) == pdTRUE);
    }

    if (self->ws_restart_pending_.exchange(false, std::memory_order_acq_rel)) {
      ESP_LOGI(TAG, "[ws] server closed the connection, restarting client");
      // The CLOSED event is dispatched from inside the client task shortly
      // before it exits; give it a beat so stop()/start() can't overlap a
      // task that is still tearing itself down.
      vTaskDelay(pdMS_TO_TICKS(100));
      websocket_force_reconnect(client);
      last_up_us = esp_timer_get_time();
      last_forced_us = last_up_us;
    }

    const uint64_t now = esp_timer_get_time();
    if (now - last_supervise_us < cfg::ws_supervise_interval_us) continue;
    last_supervise_us = now;

    if (!esp_websocket_client_is_connected(client)) {
      // Auto-reconnect (reconnect_timeout_ms) handles routine drops. Once the
      // client has been down past the stuck threshold, force restarts — but
      // spaced by the forced-retry interval, since stop() aborts an in-flight
      // connect and a slow (DNS/roam) connect needs time to complete.
      if (now - last_up_us >= cfg::ws_stuck_reconnect_us &&
          now - last_forced_us >= cfg::ws_forced_retry_interval_us) {
        ESP_LOGW(TAG, "[ws] disconnected too long, forcing reconnect");
        websocket_force_reconnect(client);
        last_forced_us = now;
      }
      continue;
    }
    last_up_us = now;

    if (self->ws_client_ && esp_websocket_client_is_connected(self->ws_client_)) {
      if (now - self->last_keepalive_us_ >= cfg::ws_keepalive_interval_us) {
        if (self->ws_send_keepalive_()) {
          self->last_keepalive_us_ = now;
          ESP_LOGV(TAG, "[ws] keepalive sent");
        }
      }
    }
  }
}

bool RemoteWebView::enqueue_out_msg_(OutMsg &m, bool evict_on_full) {
  if (!q_send_) return false;
  if (xQueueSend(q_send_, &m, 0) == pdTRUE) return true;
  if (!evict_on_full) return false;
  // Make room for packets that must not be lost (touch Down/Up, OpenURL) by
  // shedding the oldest queued packet — typically a stale Move.
  OutMsg discarded;
  if (xQueueReceive(q_send_, &discarded, 0) == pdTRUE && discarded.ext) free(discarded.ext);
  return xQueueSend(q_send_, &m, 0) == pdTRUE;
}

bool RemoteWebView::queue_ws_packet_(const uint8_t *pkt, size_t len, bool evict_on_full) {
  if (!pkt || len == 0 || len > cfg::send_msg_max_bytes) return false;
  OutMsg m;
  m.len = (uint8_t) len;
  memcpy(m.buf, pkt, len);
  return enqueue_out_msg_(m, evict_on_full);
}

void RemoteWebView::reasm_reset_(WsReasm &r) {
  if (r.buf && self_) self_->release_msg_buf_(r.buf);
  r.buf = nullptr; r.total = 0; r.filled = 0;
}

uint8_t *RemoteWebView::acquire_msg_buf_() {
  uint8_t *b = nullptr;
  if (!q_free_ || xQueueReceive(q_free_, &b, 0) != pdTRUE) return nullptr;
  return b;
}

void RemoteWebView::release_msg_buf_(uint8_t *buf) {
  if (!buf) return;
  if (!q_free_ || xQueueSend(q_free_, &buf, 0) != pdTRUE) free(buf);
}

void RemoteWebView::ws_event_handler_(void *handler_arg, esp_event_base_t, int32_t event_id, void *event_data) {
  auto *r = reinterpret_cast<WsReasm*>(handler_arg);
  auto *e = reinterpret_cast<const esp_websocket_event_data_t *>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      if (self_) self_->ws_client_ = e->client;
      ESP_LOGI(TAG, "[ws] connected");
      
      if (self_) self_->last_keepalive_us_ = esp_timer_get_time();
      if (self_ && !self_->url_.empty()) {
        self_->ws_send_open_url_(self_->url_.c_str(), 0);
      }
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      if (self_) self_->ws_client_ = nullptr;
      ESP_LOGI(TAG, "[ws] disconnected");
      if (self_) self_->last_keepalive_us_ = 0;
      reasm_reset_(*r);
      break;

#ifdef WEBSOCKET_EVENT_CLOSED
    case WEBSOCKET_EVENT_CLOSED:
      // A completed close handshake stops the client task; auto-reconnect
      // only covers error paths. Ask the WS task to restart the client —
      // calling stop() from this (the client's own event) task is unsafe.
      if (self_) {
        self_->ws_client_ = nullptr;
        self_->last_keepalive_us_ = 0;
        self_->ws_restart_pending_.store(true, std::memory_order_release);
      }
      ESP_LOGI(TAG, "[ws] closed");
      reasm_reset_(*r);
      break;
#endif

    case WEBSOCKET_EVENT_DATA: {
      if (!self_) break;

      const uint8_t *frag = (const uint8_t *)e->data_ptr;
      size_t frag_len = (size_t)e->data_len;
      bool is_bin  = (e->op_code == WS_TRANSPORT_OPCODES_BINARY);
      if (!is_bin) break;

      if (e->payload_offset == 0) {
        reasm_reset_(*r);
        const size_t max_allowed = self_->reasm_buf_size_;
        if ((size_t)e->payload_len > max_allowed) {
          ESP_LOGE(TAG, "WS message too large: %u > %u", (unsigned)e->payload_len, (unsigned)max_allowed);
          break;
        }
        r->buf = self_->acquire_msg_buf_();
        if (!r->buf) { ESP_LOGW(TAG, "no free message buffers, dropping message"); break; }
        r->total = (size_t)e->payload_len;
      }
      if (!r->buf || r->total == 0) break;

      if ((size_t)e->payload_offset + frag_len > r->total) {
        ESP_LOGE(TAG, "bad fragment bounds");
        reasm_reset_(*r);
        break;
      }
      memcpy(r->buf + e->payload_offset, frag, frag_len);
      size_t new_filled = (size_t)e->payload_offset + frag_len;
      if (new_filled > r->filled) r->filled = new_filled;

      if (r->filled == r->total) {
        WsMsg m;
        m.buf = r->buf; m.len = r->total; m.client = e->client;
        r->buf = nullptr; r->total = 0; r->filled = 0;
        if (!self_->q_decode_) {
          self_->release_msg_buf_(m.buf);
        } else if (xQueueSend(self_->q_decode_, &m, 0) != pdTRUE) {
          // Queue full: evict the oldest queued packet only if it belongs to
          // an OLDER frame — stale frames are shed, but tiles of the frame
          // still being received must survive (nothing resends them).
          WsMsg oldest;
          if (xQueueReceive(self_->q_decode_, &oldest, 0) == pdTRUE) {
            if (is_same_frame_(oldest, m)) {
              // The whole queue is this frame: depth is too small for the
              // frame's message count. Keep the older tiles, drop the newest.
              xQueueSendToFront(self_->q_decode_, &oldest, 0);
              ESP_LOGW(TAG, "decode queue full mid-frame, dropping packet (raise decode_queue_depth or max_bytes_per_msg)");
              self_->release_msg_buf_(m.buf);
            } else {
              self_->release_msg_buf_(oldest.buf);
              if (xQueueSend(self_->q_decode_, &m, 0) == pdTRUE) {
                ESP_LOGW(TAG, "decode queue full, dropped oldest packet");
              } else {
                ESP_LOGW(TAG, "decode queue full, dropping packet");
                self_->release_msg_buf_(m.buf);
              }
            }
          } else {
            self_->release_msg_buf_(m.buf);
          }
        }
      }
      break;
    }

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "[ws] error: type=%d tls_err=%d tls_stack=%d",
               e->error_handle.error_type,
               e->error_handle.esp_tls_last_esp_err,
               e->error_handle.esp_tls_stack_err);
      break;

    default: break;
  }
}

void RemoteWebView::start_decode_task_() {
  xTaskCreatePinnedToCore(&RemoteWebView::decode_task_tramp_, "rwv_decode", cfg::decode_task_stack, this, 6, &t_decode_, 1);
}

void RemoteWebView::decode_task_tramp_(void *arg) {
  auto *self = reinterpret_cast<RemoteWebView*>(arg);
  WsMsg m;
  for (;;) {
    if (xQueueReceive(self->q_decode_, &m, portMAX_DELAY) == pdTRUE) {
      self->process_packet_(m.client, m.buf, m.len);
      self->release_msg_buf_(m.buf);
    }
  }
}

void RemoteWebView::process_packet_(void * /*client*/, const uint8_t *data, size_t len) {
  if (!data || len == 0) return;

  const proto::MsgType type = (proto::MsgType)data[0];
  switch (type) {
    case proto::MsgType::Frame:
      process_frame_packet_(data, len);
      break;
    case proto::MsgType::FrameStats:
      process_frame_stats_packet_(data, len);
      break;
    case proto::MsgType::CurrentURL: // deal with packet #6 here - aka our current display URL packet
      process_current_url_packet_(data, len);
      break;
    default:
      ESP_LOGW(TAG, "unknown packet type: %d", (int)type);
      break;
  }
}

void RemoteWebView::process_frame_packet_(const uint8_t *data, size_t len)
{
  if (!data || len < sizeof(proto::FrameHeader)) return;

  proto::FrameInfo fi{};
  size_t off = 0;
  if (!proto::parse_frame_header(data, len, fi, off)) return;

  if (fi.frame_id != frame_id_) {
    frame_id_ = fi.frame_id;
    frame_tiles_= 0;
    frame_bytes_= 0;
    frame_start_us_ = esp_timer_get_time();
  }
  frame_bytes_ += len;
  frame_tiles_ += fi.tile_count;

  for (uint16_t i = 0; i < fi.tile_count; i++) {
    proto::TileHeader th{};
    if (!proto::parse_tile_header(data, len, th, off)) return;
    if (off + th.dlen > len) return;

    if (th.w == 0 || th.h == 0 || th.w > display_width_ || th.h > display_height_) {
      off += th.dlen;
      continue;
    }

    if (fi.enc == proto::Encoding::JPEG && th.dlen) {
      decode_jpeg_tile_to_lcd_((int16_t)th.x, (int16_t)th.y, data + off, th.dlen);
    }
    
    off += th.dlen;
  }

  if (fi.flags & proto::kFlafLastOfFrame) {
    const uint32_t time_ms = (esp_timer_get_time() - frame_start_us_) / 1000ULL;
    frame_stats_bytes_ += frame_bytes_;
    frame_stats_time_ += time_ms;
    frame_stats_count_++;
    // Verbose, not debug: ESPHome defaults to DEBUG and blocking UART output
    // here costs ~5 ms per frame on the decode core.
    ESP_LOGV(TAG, "frame %lu: tiles %u (%u bytes) - %lu ms", frame_id_, frame_tiles_, frame_bytes_, time_ms);

    this->frame_update_pending_.store(true, std::memory_order_release);
  }
}

void RemoteWebView::process_frame_stats_packet_(const uint8_t *data, size_t len)
{
  uint32_t avg_render_time = 0;
  if (frame_stats_count_ > 0)
    avg_render_time = frame_stats_time_ / frame_stats_count_;

  ESP_LOGD(TAG, "sending frame stats: avg_time=%u ms, bytes=%u", (unsigned)avg_render_time, (unsigned)frame_stats_bytes_);
  uint8_t pkt[sizeof(proto::FrameStatsPacket)];
  const size_t n = proto::build_frame_stats_packet(avg_render_time, frame_stats_bytes_, pkt);

  frame_stats_time_ = 0;
  frame_stats_count_ = 0;
  frame_stats_bytes_ = 0;

  // Queued for the WS task — a synchronous send here would stall tile decoding.
  if (!queue_ws_packet_(pkt, n))
    ESP_LOGW(TAG, "send queue full, dropping frame stats");
}

bool RemoteWebView::decode_jpeg_tile_to_lcd_(int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len) {
  if (!data || !len) return false;

#if REMOTE_WEBVIEW_HW_JPEG
  if (hw_dec_ && hw_decode_input_buf_ && hw_decode_output_buf_) {
    jpeg_decode_picture_info_t hdr{};
    if (jpeg_decoder_get_info(data, (uint32_t)len, &hdr) != ESP_OK || !hdr.width || !hdr.height) {
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }

    const int aligned_w = (hdr.width  + 15) & ~15;
    const int aligned_h = (hdr.height + 15) & ~15;
    const uint32_t out_sz = (uint32_t)aligned_w * (uint32_t)aligned_h * 2u;

    if (aligned_w != (int)hdr.width) {
      ESP_LOGW(TAG, "jpeg dimensions not aligned: %u x %u", (unsigned)hdr.width, (unsigned)hdr.height);
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }
    
    if (len > hw_decode_input_size_ || out_sz > hw_decode_output_size_) {
      ESP_LOGW(TAG, "tile too large for HW decoder buffers");
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }

    jpeg_decode_cfg_t jcfg{};
    jcfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    jcfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    jcfg.conv_std      = JPEG_YUV_RGB_CONV_STD_BT709;

    memcpy(hw_decode_input_buf_, data, len);
    
    uint32_t written = 0;
    esp_err_t dr = jpeg_decoder_process(hw_dec_, &jcfg, hw_decode_input_buf_, (uint32_t)len, 
                                        hw_decode_output_buf_, (uint32_t)hw_decode_output_size_, &written);

    if (dr != ESP_OK) {
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }

    display_->draw_pixels_at(dst_x, dst_y, (int)hdr.width, (int)hdr.height, hw_decode_output_buf_,
        esphome::display::COLOR_ORDER_RGB,
        esphome::display::COLOR_BITNESS_565,
        rgb565_big_endian_);

    return true;
  }
#endif  // REMOTE_WEBVIEW_HW_JPEG

  return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
}

bool RemoteWebView::decode_jpeg_tile_software_(int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len) {
  if (!jd_.openRAM((uint8_t*)data, (int)len, &RemoteWebView::jpeg_draw_cb_s_)) {
    ESP_LOGE(TAG, "openRAM failed (len=%u) err=%d", (unsigned)len, jd_.getLastError());
    return false;
  }

  jd_.setMaxOutputSize(8 * 2048);
  jd_.setPixelType(rgb565_big_endian_ ? RGB565_BIG_ENDIAN : RGB565_LITTLE_ENDIAN);

  const int rc = jd_.decode(dst_x, dst_y, 0);
  if (rc == 0) {
    ESP_LOGE(TAG, "decode rc=%d err=%d", rc, jd_.getLastError());
    jd_.close();
    return false;
  }
  jd_.close();
  return true;
}

int RemoteWebView::jpeg_draw_cb_s_(JPEGDRAW *p) {
  return self_ ? self_->jpeg_draw_cb_(p) : 0;
}

int RemoteWebView::jpeg_draw_cb_(JPEGDRAW *p) {
  int32_t x = p->x, y = p->y, w = p->iWidth, h = p->iHeight;
  
  if (x >= display_width_ || y >= display_height_) return 1;
  if (x + w > display_width_) w = display_width_ - x;
  if (y + h > display_height_) h = display_height_ - y;
  if (w <= 0 || h <= 0) return 1;

  display_->draw_pixels_at(
      x, y, w, h,
      (const uint8_t *)p->pPixels,
      esphome::display::COLOR_ORDER_RGB,
      esphome::display::COLOR_BITNESS_565,
      rgb565_big_endian_
  );

  return 1;
}

bool RemoteWebView::ws_send_touch_event_(proto::TouchType type, int x, int y, uint8_t pid) {
  if (touch_disabled_)
    return false;

  if (!ws_client_ || !esp_websocket_client_is_connected(ws_client_))
    return false;

  if (x < 0) x = 0; if (y < 0) y = 0;
  if (x > 65535) x = 65535; if (y > 65535) y = 65535;

  uint8_t pkt[sizeof(proto::TouchPacket)];
  const size_t n = proto::build_touch_packet(type, pid, x, y, pkt);

  // Queued for the WS task — a slow socket must never stall the main loop.
  // Down/Up evict a stale queued packet rather than be dropped: losing an
  // Up leaves the remote page stuck in a phantom drag.
  const bool must_deliver = (type != proto::TouchType::Move);
  return queue_ws_packet_(pkt, n, must_deliver);
}

bool RemoteWebView::ws_send_open_url_(const char *url, uint16_t flags) {
  if (!ws_client_ || !url || !esp_websocket_client_is_connected(ws_client_))
    return false;

  const uint32_t n = (uint32_t) strlen(url);
  const size_t total = sizeof(proto::OpenURLHeader) + (size_t) n;

  if (total > 16 * 1024) return false;

  auto *pkt = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pkt) pkt = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_8BIT);
  if (!pkt) return false;

  const size_t written = proto::build_open_url_packet(url, flags, pkt, total);
  if (!written) {
    free(pkt);
    return false;
  }

  // Queued for the WS task like every other producer-side send — automations
  // call open_url() on the main loop, which must never block on the socket.
  OutMsg m;
  m.ext = pkt;
  m.ext_len = written;
  if (enqueue_out_msg_(m, true)) return true;
  free(pkt);
  return false;
}

bool RemoteWebView::ws_send_keepalive_() {
  if (!ws_client_ || !ws_send_mtx_ || !esp_websocket_client_is_connected(ws_client_))
    return false;

  uint8_t pkt[sizeof(proto::KeepalivePacket)];
  const size_t n = proto::build_keepalive_packet(pkt);
  if (!n) return false;

  const TickType_t to = pdMS_TO_TICKS(50);
  if (xSemaphoreTake(ws_send_mtx_, to) != pdTRUE)
    return false;

  const int r = esp_websocket_client_send_bin(ws_client_, (const char*)pkt, (int)n, to);
  xSemaphoreGive(ws_send_mtx_);
  return r == (int)n;
}

void RemoteWebViewTouchListener::update(const touchscreen::TouchPoints_t &pts) {
  if (!parent_) return;

  const uint64_t now = esp_timer_get_time();
  for (auto &p : pts) {
    // STATE_RELEASING is a flag OR'd into the state (e.g. PRESSED|RELEASING),
    // so it must be tested as a bit, never as a switch case.
    if (p.state & touchscreen::STATE_RELEASING) {
      parent_->ws_send_touch_event_(proto::TouchType::Up, p.x, p.y, p.id);
      up_sent_ = true;
      continue;
    }
    switch (p.state) {
      case touchscreen::STATE_PRESSED:
        parent_->ws_send_touch_event_(proto::TouchType::Down, p.x, p.y, p.id);
        last_x_ = p.x; last_y_ = p.y; last_id_ = p.id;
        up_sent_ = false;
        break;
      case touchscreen::STATE_UPDATED:
        if (!RemoteWebView::kCoalesceMoves || RemoteWebView::kMoveIntervalUs == 0 ||
            (now - parent_->last_move_us_) >= RemoteWebView::kMoveIntervalUs) {
          parent_->last_move_us_ = now;
          parent_->ws_send_touch_event_(proto::TouchType::Move, p.x, p.y, p.id);
        }
        last_x_ = p.x; last_y_ = p.y; last_id_ = p.id;
        break;
      default: break;
    }
  }
}

void RemoteWebViewTouchListener::release() {
  if (!parent_ || up_sent_) return;

  // Fallback if no RELEASING point was delivered: lift at the last known
  // position, not (0,0) — the server maps Up to a click/drag-end location.
  parent_->ws_send_touch_event_(proto::TouchType::Up, last_x_, last_y_, last_id_);
  up_sent_ = true;
}

void RemoteWebViewTouchListener::touch(touchscreen::TouchPoint tp) {
  // Intentionally empty: ESPHome invokes both touch() and update() for the
  // first press (update() sees STATE_PRESSED), so sending here duplicated
  // every Down. update() is the single source of touch packets.
}

void RemoteWebView::disable_touch(bool disable) {
  touch_disabled_ = disable;
  ESP_LOGD(TAG, "touch %s", disable ? "disabled" : "enabled");
}

void RemoteWebView::set_server(const std::string &s) {
  auto pos = s.rfind(':');
  if (pos == std::string::npos || pos == s.size() - 1) {
    ESP_LOGE(TAG, "server must be host:port, got: %s", s.c_str());
    return;
  }
  server_host_ = s.substr(0, pos);
  server_port_ = atoi(s.c_str() + pos + 1);
  if (server_port_ <= 0 || server_port_ > 65535) {
    ESP_LOGE(TAG, "invalid port in server: %s", s.c_str());
    server_host_.clear();
    server_port_ = 0;
  }
}

void RemoteWebView::append_q_int_(std::string &s, const char *k, int v) {
  if (v < 0) return;
  s += (s.find('?') == std::string::npos) ? '?' : '&';
  char buf[32];
  snprintf(buf, sizeof(buf), "%s=%d", k, v);
  s += buf;
}

void RemoteWebView::append_q_float_(std::string &s, const char *k, float v) {
  if (v < 0.0f) return;
  s += (s.find('?') == std::string::npos) ? '?' : '&';
  char buf[32];
  
  snprintf(buf, sizeof(buf), "%s=%.2f", k, (double)v);
  s += buf;
}

void RemoteWebView::append_q_str_(std::string &s, const char *k, const char *v) {
  if (!v || !*v) return;
  s += (s.find('?') == std::string::npos) ? '?' : '&';
  s += k; s += '='; s += v;
}

std::string RemoteWebView::resolve_device_id_() const {
  if (!device_id_.empty()) return device_id_;

  uint8_t mac[6] = {0};
  esp_err_t err = ESP_FAIL;
  
#if ESP_IDF_VERSION_MAJOR >= 5
  err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (err != ESP_OK) {
    err = esp_read_mac(mac, ESP_MAC_BT);
  }
  if (err != ESP_OK) {
    err = esp_read_mac(mac, ESP_MAC_ETH);
  }
  if (err != ESP_OK) {
    err = esp_efuse_mac_get_default(mac);
  }
#else
  err = esp_efuse_mac_get_default(mac);
  if (err != ESP_OK) {
    err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  }
#endif

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to read MAC address, using random ID");
    snprintf((char*)mac, sizeof(mac), "%06lx", (unsigned long)esp_random());
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "esp32-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

std::string RemoteWebView::build_ws_uri_() const {
  std::string uri;
  uri = "ws://" + server_host_ + ":" + std::to_string(server_port_);
  uri += "/";

  const std::string id = resolve_device_id_();
  append_q_str_(uri, "id", id.c_str());

  append_q_int_(uri, "w", display_width_);
  append_q_int_(uri, "h", display_height_);

  append_q_int_(uri,   "r",    rotation_);
  append_q_int_(uri,   "ts",   tile_size_);
  append_q_int_(uri,   "fftc", full_frame_tile_count_);
  append_q_float_(uri, "ffat", full_frame_area_threshold_);
  append_q_int_(uri,   "ffe",  full_frame_every_);
  append_q_int_(uri,   "enf",  every_nth_frame_);
  append_q_int_(uri,   "mfi",  min_frame_interval_);
  append_q_int_(uri,   "q",    jpeg_quality_);
  append_q_int_(uri,   "mbpm", max_bytes_per_msg_);

  return uri;
}

}  // namespace remote_webview
}  // namespace esphome
