#pragma once
#include "esphome/core/component.h"
#include "esphome/components/display/display.h"
#include "esphome/components/touchscreen/touchscreen.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/helpers.h"
#include "JPEGDEC.h"
#include "protocol.h"
#include "remote_webview_config.h"

#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <atomic>

#if defined(CONFIG_IDF_TARGET_ESP32P4)
  #include "driver/jpeg_decode.h"
  #define REMOTE_WEBVIEW_HW_JPEG 1
#else
  #define REMOTE_WEBVIEW_HW_JPEG 0
#endif
// No manual cache msync is needed around the HW decoder: jpeg_decoder_process
// syncs the input (C2M) and invalidates the output (M2C) itself.

namespace esphome {
namespace remote_webview {

class RemoteWebView : public Component {
 public:
  void set_display(display::Display *d) { display_ = d; }
  void set_touchscreen(touchscreen::Touchscreen *t) { touch_ = t; }
  void set_device_id(const std::string &s) { device_id_ = s; }
  void set_url(const std::string &s) { url_ = s; }
  void set_server(const std::string &s);
  void set_tile_size(int v) { tile_size_ = v; }
  void set_full_frame_tile_count(int v) { full_frame_tile_count_ = v; }
  void set_full_frame_area_threshold(float v) { full_frame_area_threshold_ = v; }
  void set_full_frame_every(int v) { full_frame_every_ = v; }
  void set_every_nth_frame(int v) { every_nth_frame_ = v; }
  void set_min_frame_interval(int v) { min_frame_interval_ = v; }
  void set_jpeg_quality(int v) { jpeg_quality_ = v; }
  void set_max_bytes_per_msg(int v) { max_bytes_per_msg_ = v; }
  void set_big_endian(bool v) { rgb565_big_endian_ = v; }
  void set_rotation(int v) { rotation_ = v; }
  void disable_touch(bool disable);
  bool open_url(const std::string &s);
  std::string get_current_url() const;
  void set_url_sensor(text_sensor::TextSensor *s) { url_sensor_ = s; }
  void add_on_frame_update_callback(std::function<void()> &&callback);
  void trigger_on_frame_update();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 private:
  struct WsMsg {
    uint8_t *buf{nullptr};
    size_t   len{0};
    void    *client{nullptr}; // opaque esp_websocket_client_handle_t
  };
  struct WsReasm {
    uint8_t *buf{nullptr};
    size_t total{0}, filled{0};
  };
  struct OutMsg {
    uint8_t len{0};
    uint8_t buf[cfg::send_msg_max_bytes]{};
    // Payloads larger than buf (OpenURL) travel as an owned heap block that
    // the WS task frees after sending (or on eviction/drop).
    uint8_t *ext{nullptr};
    size_t ext_len{0};
  };

  CallbackManager<void()> on_frame_update_callback_{};

  static constexpr bool     kCoalesceMoves  = cfg::coalesce_moves;
  static constexpr uint32_t kMoveRateHz     = cfg::move_rate_hz;
  static constexpr uint32_t kMoveIntervalUs = (kMoveRateHz ? (1000000u / kMoveRateHz) : 0);

  static RemoteWebView *self_;
  display::Display *display_{nullptr};
  touchscreen::Touchscreen *touch_ = nullptr;
  class RemoteWebViewTouchListener *touch_listener_ = nullptr;

  int display_width_{0};
  int display_height_{0};
  std::string url_;
  std::string server_host_;
  std::string device_id_;
  int server_port_{0};
  int tile_size_{-1};
  int full_frame_tile_count_{-1};
  float full_frame_area_threshold_{-1.0f};
  int full_frame_every_{-1};
  int every_nth_frame_{-1};
  int min_frame_interval_{-1};
  int jpeg_quality_{-1};
  int max_bytes_per_msg_{-1};
  bool rgb565_big_endian_{true};
  int rotation_{0};
  bool touch_disabled_{false};

#if REMOTE_WEBVIEW_HW_JPEG
  jpeg_decoder_handle_t hw_dec_{nullptr};
  uint8_t *hw_decode_input_buf_{nullptr};
  uint8_t *hw_decode_output_buf_{nullptr};
  size_t hw_decode_input_size_{0};
  size_t hw_decode_output_size_{0};
  // Fallback conditions are per-config and repeat every frame — warn once.
  bool hw_warned_unaligned_{false};
  bool hw_warned_tile_size_{false};
#endif

  uint64_t last_move_us_{0};
  uint64_t last_keepalive_us_{0};
  
  uint64_t frame_start_us_ = 0;
  uint32_t frame_id_{0xffffffffu};
  uint16_t frame_tiles_{0};
  size_t   frame_bytes_{0};
  // 64-bit: these accumulate until the server polls stats, which may be never.
  uint64_t frame_stats_time_{0};
  uint32_t frame_stats_count_{0};
  uint64_t frame_stats_bytes_{0};

  QueueHandle_t     q_decode_{nullptr};
  QueueHandle_t     q_free_{nullptr};
  QueueHandle_t     q_send_{nullptr};
  size_t            reasm_buf_size_{0};
  SemaphoreHandle_t ws_send_mtx_{nullptr};
  TaskHandle_t      t_ws_{nullptr};
  TaskHandle_t      t_decode_{nullptr};

  esp_websocket_client_handle_t ws_client_{nullptr};

  std::atomic<bool> frame_update_pending_{false};
  std::atomic<bool> url_publish_pending_{false};
  std::atomic<bool> ws_restart_pending_{false};
  std::string pending_url_{};
  SemaphoreHandle_t state_mtx_{nullptr};

  uint32_t last_trigger_ms_{0};
  text_sensor::TextSensor *url_sensor_{nullptr};

  void start_ws_task_();
  void start_decode_task_();
  static void ws_task_tramp_(void *arg);
  static void decode_task_tramp_(void *arg);

  static void ws_event_handler_(void *handler_arg, esp_event_base_t base, int32_t event_id, void *event_data);
  static void reasm_reset_(WsReasm &r);
  static bool is_same_frame_(const WsMsg &a, const WsMsg &b);
  uint8_t *acquire_msg_buf_();
  void release_msg_buf_(uint8_t *buf);

  void process_packet_(void *client, const uint8_t *data, size_t len);
  void process_frame_packet_(const uint8_t *data, size_t len);
  void process_frame_stats_packet_(const uint8_t *data, size_t len);
  bool decode_jpeg_tile_to_lcd_(int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len);
  bool decode_jpeg_tile_software_(int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len);

  static int jpeg_draw_cb_s_(JPEGDRAW *p);
  int jpeg_draw_cb_(JPEGDRAW *p);
  JPEGDEC jd_;

  static bool out_msg_droppable_(const OutMsg &m);
  bool enqueue_out_msg_(OutMsg &m, bool evict_on_full);
  bool queue_ws_packet_(const uint8_t *pkt, size_t len, bool evict_on_full = false);
  bool ws_send_touch_event_(proto::TouchType type, int x, int y, uint8_t pid);
  bool ws_send_keepalive_();
  bool ws_send_open_url_(const char *url, uint16_t flags);

  std::string resolve_device_id_() const;
  std::string build_ws_uri_() const;
  static void append_q_int_(std::string &s, const char *k, int v);
  static void append_q_float_(std::string &s, const char *k, float v);
  static void append_q_str_(std::string &s, const char *k, const char *v);

  void process_current_url_packet_(const uint8_t *data, size_t len);

  friend class RemoteWebViewTouchListener;
};

class RemoteWebViewTouchListener : public touchscreen::TouchListener {
 public:
  explicit RemoteWebViewTouchListener(RemoteWebView *p) : parent_(p) {}
  void touch(touchscreen::TouchPoint tp) override;
  void update(const touchscreen::TouchPoints_t &pts) override;
  void release() override;
 private:
  // Per-pointer state: ESPHome may lift one finger without delivering an
  // update (coords of the remaining finger unchanged), so each pointer's
  // last position and down/up state must be tracked individually.
  static constexpr int kMaxPointers = 5;
  struct PointerSlot {
    bool active{false};
    uint8_t id{0};
    int16_t x{0}, y{0};
  };
  PointerSlot *find_slot_(uint8_t id);
  PointerSlot *claim_slot_(uint8_t id);
  RemoteWebView *parent_{nullptr};
  PointerSlot pointers_[kMaxPointers];
};

}  // namespace remote_webview
}  // namespace esphome
