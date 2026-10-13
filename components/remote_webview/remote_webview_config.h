#pragma once

namespace esphome {
namespace remote_webview {
namespace cfg {

// Task stacks live in internal DRAM. JPEGDEC keeps its ~25 KB state in the
// heap-allocated jd_ member, not on the stack, so 16 KB is ample — verify
// with uxTaskGetStackHighWaterMark if the decode path grows.
inline constexpr int decode_task_stack = 16 * 1024;
// Same priority as ESPHome's loopTask (also on CPU 1), so a continuous stream of frames
// can't starve the main loop and trigger the task watchdog.
inline constexpr int decode_task_prio = 1;
inline constexpr int ws_task_stack = 8 * 1024;
inline constexpr int ws_task_prio = 5;
// Kept short so a slow decoder fills the queue quickly and backpressure reaches the server
// (via TCP flow control) before stale frames pile up on the device.
inline constexpr int decode_queue_depth = 4;
// Message-buffer pool: queue depth + one buffer being reassembled (or waiting
// for queue space) + one being decoded
inline constexpr int msg_pool_extra = 2;
// Without PSRAM the pool may take at most this much internal RAM in total —
// WiFi/lwIP and task stacks need the rest of the heap. With the default
// 64 KB message size this yields no buffers at all: non-PSRAM targets must
// set max_bytes_per_msg small enough for several buffers to fit.
inline constexpr size_t msg_pool_max_internal_bytes = 48 * 1024;

inline constexpr size_t ws_max_message_bytes = 64 * 1024;
inline constexpr size_t ws_buffer_size = 30 * 1024;
inline constexpr size_t ws_keepalive_interval_us = 60 * 1000 * 1000;
inline constexpr int ws_reconnect_timeout_ms = 2000;
inline constexpr int ws_network_timeout_ms = 10000;
inline constexpr int ws_init_retry_ms = 5000;
// How often the WS task wakes without a notification (keepalive, watchdog).
inline constexpr int ws_task_poll_ms = 250;
// Watchdog: force a client restart only after auto-reconnect has been
// down this long — routine reconnects are the client library's job.
inline constexpr uint64_t ws_stuck_reconnect_us = 30ull * 1000 * 1000;
// Give each forced restart longer than ws_network_timeout_ms to
// establish before forcing again — stop() aborts an in-flight connect,
// so retrying faster than a slow connect completes would livelock.
inline constexpr uint64_t ws_forced_retry_interval_us = 15ull * 1000 * 1000;

// Outbound queue for small fixed-size packets (touch, frame ack, frame stats),
// drained by the WS task so producers never block on the socket.
inline constexpr int send_queue_depth = 16;
inline constexpr size_t send_msg_max_bytes = 16;

// How long the WS handler waits for space in the decode queue before shedding a message.
// While it waits the socket isn't read, so TCP flow control holds back the server, which
// skips frames while the socket has unsent data. Shedding is the last resort (it triggers
// a full-frame request). Newer esp_websocket_client versions call the handler with the
// client lock held; sends don't wait on it because __init__.py enables
// CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK.
inline constexpr int decode_enqueue_timeout_ms = 1000;
// Minimum interval between "decode queue full" log lines.
inline constexpr int64_t drop_log_interval_us = 5 * 1000 * 1000;
// Frames finishing this long after a touch Up count towards touch-to-frame latency stats.
inline constexpr int64_t touch_latency_window_us = 2 * 1000 * 1000;
// Minimum interval between full-frame requests while the screen is out of sync.
inline constexpr int64_t full_frame_request_interval_us = 2 * 1000 * 1000;

inline constexpr bool coalesce_moves = true;
inline constexpr uint32_t move_rate_hz = 60;

} // namespace cfg
} // namespace remote_webview
} // namespace esphome
