export class CanvasRenderer {
  private readonly canvas: HTMLCanvasElement;
  private readonly ctx: CanvasRenderingContext2D;

  constructor(canvas: HTMLCanvasElement, width: number, height: number) {
    this.canvas = canvas;
    this.canvas.width = width;
    this.canvas.height = height;

    const ctx = this.canvas.getContext("2d", { alpha: false });
    if (!ctx) {
      throw new Error("Cannot initialize 2D context");
    }

    this.ctx = ctx;
    this.clear();
  }

  resize(width: number, height: number): void {
    this.canvas.width = width;
    this.canvas.height = height;
    this.clear();
  }

  clear(): void {
    this.ctx.fillStyle = "#111";
    this.ctx.fillRect(0, 0, this.canvas.width, this.canvas.height);
  }

  decodeJpegTile(tileData: Uint8Array<ArrayBuffer>): Promise<ImageBitmap> {
    // Blob respects the view's offset/length and copies internally — no slice needed.
    const blob = new Blob([tileData], { type: "image/jpeg" });
    return createImageBitmap(blob);
  }

  drawBitmap(bitmap: ImageBitmap, x: number, y: number, w: number, h: number): void {
    this.ctx.drawImage(bitmap, 0, 0, bitmap.width, bitmap.height, x, y, w, h);
    bitmap.close();
  }
}
