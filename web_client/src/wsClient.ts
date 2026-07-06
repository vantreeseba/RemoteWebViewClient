import {
  buildKeepalivePacket,
  buildOpenUrlPacket,
  buildTouchPacket,
  buildWsUri,
  Encoding,
  FLAG_LAST_OF_FRAME,
  MsgType,
  parseFramePacket,
  parseCurrentURLPacket,
  QueryOptions,
  TouchType
} from "./protocol";
import { CanvasRenderer } from "./canvasRenderer";

const KEEPALIVE_INTERVAL_MS = 60_000;
const MOVE_INTERVAL_MS = 1000 / 60;
const MAX_INBOUND_QUEUE = 32;

export type Metrics = {
  status: string;
  frames: number;
  bytes: number;
  lastFrameId: number;
  lastError: string;
};

type Handlers = {
  onMetrics: (metrics: Metrics) => void;
  onURL: (url: string) => void;
};

export class RemoteWebViewBrowserClient {
  private ws: WebSocket | null = null;
  private readonly renderer: CanvasRenderer;
  private readonly handlers: Handlers;

  private readonly inboundQueue: ArrayBuffer[] = [];
  private processing = false;
  // Bumped on disconnect; in-flight async work checks it after every await
  // so a completed decode can't draw or push metrics for a dead connection.
  private generation = 0;
  private reconnectDelayMs = 1000;

  // One id per client instance: reconnects must not register as new devices,
  // since every abandoned session lives on the server until idle cleanup.
  private readonly clientId = `browser-${crypto.randomUUID().slice(0, 8)}`;
  private maxBytesPerMsg = 64 * 1024;
  private keepaliveId: number | null = null;
  private reconnectId: number | null = null;

  private frames = 0;
  private bytes = 0;
  private lastFrameId = -1;
  private lastError = "-";

  private lastMoveAt = 0;

  constructor(renderer: CanvasRenderer, handlers: Handlers) {
    this.renderer = renderer;
    this.handlers = handlers;
  }

  connect(server: string, options: QueryOptions): void {
    this.disconnect();

    const opts: QueryOptions = { ...options, id: options.id ?? this.clientId };

    if (opts.mbpm && opts.mbpm > 0) {
      this.maxBytesPerMsg = opts.mbpm;
    }

    const uri = buildWsUri(server, opts);
    const ws = new WebSocket(uri);
    ws.binaryType = "arraybuffer";
    // Assign before any handler can fire, so disconnect() can tear down a
    // socket that is still CONNECTING — otherwise its onclose fires later
    // and schedules a reconnect the user just cancelled.
    this.ws = ws;

    ws.onopen = () => {
      // Backoff is NOT reset here: a server that accepts the handshake and
      // immediately drops would otherwise be hammered at 1 s forever. It
      // resets when the first frame actually arrives.
      this.startKeepalive();
      this.pushMetrics("connected");
    };

    ws.onclose = () => {
      this.stopKeepalive();
      this.pushMetrics("disconnected");
      this.scheduleReconnect(server, opts);
    };

    ws.onerror = () => {
      this.lastError = "websocket error";
      this.pushMetrics("error");
    };

    ws.onmessage = (event) => {
      if (!(event.data instanceof ArrayBuffer)) {
        return;
      }

      if (event.data.byteLength > this.maxBytesPerMsg) {
        this.lastError = `message too large: ${event.data.byteLength}`;
        this.pushMetrics("warning");
        return;
      }

      this.bytes += event.data.byteLength;
      this.inboundQueue.push(event.data);
      // Backpressure: when decode falls behind, shed the oldest packets so
      // the display tracks real time instead of drifting ever further back.
      while (this.inboundQueue.length > MAX_INBOUND_QUEUE) {
        this.inboundQueue.shift();
      }
      this.drainQueue();
    };
  }

  disconnect(): void {
    if (this.reconnectId !== null) {
      clearTimeout(this.reconnectId);
      this.reconnectId = null;
    }

    this.stopKeepalive();

    if (this.ws) {
      this.ws.onopen = null;
      this.ws.onclose = null;
      this.ws.onerror = null;
      this.ws.onmessage = null;
      this.ws.close();
      this.ws = null;
    }

    this.inboundQueue.length = 0;
    this.generation += 1;
    this.processing = false;
  }

  sendOpenUrl(url: string): boolean {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      return false;
    }

    const payload = buildOpenUrlPacket(url);
    this.ws.send(payload);
    return true;
  }

  sendTouch(type: TouchType, pointerId: number, x: number, y: number): void {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      return;
    }

    const now = performance.now();
    if (type === TouchType.Move && now - this.lastMoveAt < MOVE_INTERVAL_MS) {
      return;
    }

    if (type === TouchType.Move) {
      this.lastMoveAt = now;
    }

    const payload = buildTouchPacket(type, pointerId, x, y);
    this.ws.send(payload);
  }

  private scheduleReconnect(server: string, options: QueryOptions): void {
    if (this.reconnectId !== null) {
      clearTimeout(this.reconnectId);
    }

    this.reconnectId = window.setTimeout(() => {
      this.connect(server, options);
    }, this.reconnectDelayMs);

    this.reconnectDelayMs = Math.min(this.reconnectDelayMs * 2, 15_000);
  }

  private startKeepalive(): void {
    this.stopKeepalive();
    this.keepaliveId = window.setInterval(() => {
      if (this.ws && this.ws.readyState === WebSocket.OPEN) {
        this.ws.send(buildKeepalivePacket());
      }
    }, KEEPALIVE_INTERVAL_MS);
  }

  private stopKeepalive(): void {
    if (this.keepaliveId !== null) {
      clearInterval(this.keepaliveId);
      this.keepaliveId = null;
    }
  }

  private async drainQueue(): Promise<void> {
    if (this.processing) {
      return;
    }

    this.processing = true;
    const gen = this.generation;
    try {
      while (gen === this.generation && this.inboundQueue.length > 0) {
        const buffer = this.inboundQueue.shift() as ArrayBuffer;
        await this.processPacket(buffer, gen);
      }
    } finally {
      // A stale loop must not clear the flag out from under its successor.
      if (gen === this.generation) {
        this.processing = false;
      }
    }
  }

  private async processPacket(buffer: ArrayBuffer, gen: number): Promise<void> {
    const view = new DataView(buffer);
    const type = view.getUint8(0);

    if (type === MsgType.Frame) {
      const parsed = parseFramePacket(buffer);
      if (!parsed) {
        this.lastError = "bad frame packet";
        this.pushMetrics("warning");
        return;
      }

      if (parsed.header.encoding !== Encoding.JPEG) {
        this.lastError = `unsupported encoding: ${parsed.header.encoding}`;
        this.pushMetrics("warning");
        return;
      }

      // A real frame proves the connection is healthy — reset the backoff.
      this.reconnectDelayMs = 1000;

      const tiles = parsed.tiles.filter((tile) => tile.w > 0 && tile.h > 0);
      // Decode all tiles concurrently, then draw in packet order. allSettled
      // so one corrupt tile neither discards its siblings nor leaks their
      // decoded bitmaps (drawBitmap closes each one).
      const results = await Promise.allSettled(tiles.map((tile) => this.renderer.decodeJpegTile(tile.data)));
      if (gen !== this.generation) {
        results.forEach((result) => {
          if (result.status === "fulfilled") {
            result.value.close();
          }
        });
        return;
      }
      let failed = 0;
      results.forEach((result, i) => {
        if (result.status === "fulfilled") {
          const tile = tiles[i];
          this.renderer.drawBitmap(result.value, tile.x, tile.y, tile.w, tile.h);
        } else {
          failed += 1;
        }
      });
      if (failed > 0) {
        this.lastError = `tile decode failed (${failed}/${tiles.length})`;
        this.pushMetrics("warning");
      }

      if (parsed.header.flags & FLAG_LAST_OF_FRAME) {
        this.frames += 1;
      }
      this.lastFrameId = parsed.header.frameId;
      this.pushMetrics("connected");
      return;
    }

    if (type === MsgType.FrameStats) {
      this.pushMetrics("connected");
      return;
    }

    if (type === MsgType.CurrentURL) {
      const parsed = parseCurrentURLPacket(buffer);
      if (!parsed) {
        this.lastError = "bad current URL packet";
        this.pushMetrics("warning");
        return;
      }
      this.handlers.onURL(parsed.url);
      return;
    }

    this.lastError = `unknown packet type: ${type}`;
    this.pushMetrics("warning");
  }

  private pushMetrics(status: string): void {
    this.handlers.onMetrics({
      status,
      frames: this.frames,
      bytes: this.bytes,
      lastFrameId: this.lastFrameId,
      lastError: this.lastError
    });
  }
}
