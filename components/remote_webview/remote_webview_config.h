#pragma once

namespace esphome {
namespace remote_webview {
namespace cfg {

inline constexpr int decode_task_stack = 32 * 1024;
inline constexpr int ws_task_stack = 8 * 1024;
inline constexpr int ws_task_prio = 5;
// Must hold one full frame's burst: ceil(max full-frame bytes / mbpm) + slack.
// Frame-aware eviction keeps older frames from accumulating, so depth here
// costs memory, not standing latency.
inline constexpr int decode_queue_depth = 8;
// Message-buffer pool: queue depth + one buffer being reassembled + one being decoded
inline constexpr int msg_pool_extra = 2;
// Without PSRAM the pool may take at most this much internal RAM in total —
// WiFi/lwIP and task stacks need the rest of the heap. With the default
// 64 KB message size this yields no buffers at all: non-PSRAM targets must
// set max_bytes_per_msg small enough for several buffers to fit.
inline constexpr size_t msg_pool_max_internal_bytes = 48 * 1024;

inline constexpr size_t ws_max_message_bytes = 64 * 1024;
inline constexpr size_t ws_buffer_size = 30 * 1024;
inline constexpr size_t ws_keepalive_interval_us = 60 * 1000 * 1000;
inline constexpr uint32_t ws_supervise_interval_us = 5 * 1000 * 1000;
// Watchdog: force a client restart only after auto-reconnect has been
// down this long — routine reconnects are the client library's job.
inline constexpr uint64_t ws_stuck_reconnect_us = 30ull * 1000 * 1000;
// Give each forced restart longer than network_timeout_ms (10 s) to
// establish before forcing again — stop() aborts an in-flight connect,
// so retrying faster than a slow connect completes would livelock.
inline constexpr uint64_t ws_forced_retry_interval_us = 15ull * 1000 * 1000;

// Outbound queue for small fixed-size packets (touch, frame stats),
// drained by the WS task so producers never block on the socket.
inline constexpr int send_queue_depth = 16;
inline constexpr size_t send_msg_max_bytes = 16;

inline constexpr bool coalesce_moves = true;
inline constexpr uint32_t move_rate_hz = 60;

} // namespace cfg
} // namespace remote_webview
} // namespace esphome
