#pragma once

namespace esphome {
namespace remote_webview {
namespace cfg {

inline constexpr int decode_task_stack = 32 * 1024;
inline constexpr int ws_task_stack = 8 * 1024;
inline constexpr int ws_task_prio = 5;
inline constexpr int decode_queue_depth = 12;
// Message-buffer pool: queue depth + one buffer being reassembled + one being decoded
inline constexpr int msg_pool_extra = 2;
// Without PSRAM the pool may take at most this many buffers from internal
// RAM — WiFi/lwIP and task stacks need the rest of the heap.
inline constexpr int msg_pool_max_internal_bufs = 2;

inline constexpr size_t ws_max_message_bytes = 64 * 1024;
inline constexpr size_t ws_buffer_size = 30 * 1024;
inline constexpr size_t ws_keepalive_interval_us = 60 * 1000 * 1000;
inline constexpr uint32_t ws_supervise_interval_us = 5 * 1000 * 1000;
// Watchdog: force a client restart only after auto-reconnect has been
// down this long — routine reconnects are the client library's job.
inline constexpr uint64_t ws_stuck_reconnect_us = 30ull * 1000 * 1000;

// Outbound queue for small fixed-size packets (touch, frame stats),
// drained by the WS task so producers never block on the socket.
inline constexpr int send_queue_depth = 16;
inline constexpr size_t send_msg_max_bytes = 16;

inline constexpr bool coalesce_moves = true;
inline constexpr uint32_t move_rate_hz = 60;

} // namespace cfg
} // namespace remote_webview
} // namespace esphome
