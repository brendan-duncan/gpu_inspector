// Renders assets/icon.svg into the raster icons Electron needs: assets/icon.png (window icon on
// Linux, 512x512) and assets/icon.ico (window/taskbar icon on Windows, 16..256 px). Uses Electron
// itself to rasterize the SVG so no image tooling is required.
//
// Usage: npm run icons        (runs: electron tools/make_icons.mjs)
import { app, BrowserWindow } from "electron";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const assets = path.join(__dirname, "..", "assets");
const svgPath = path.join(assets, "icon.svg");
const ICO_SIZES = [16, 24, 32, 48, 64, 128, 256];
const PNG_SIZE = 512;

async function render(win, svg, size) {
  const html = `<!doctype html><html><head><style>
    html,body{margin:0;padding:0;background:transparent;overflow:hidden}
    img{display:block;width:${size}px;height:${size}px}
  </style></head><body><img src="data:image/svg+xml;base64,${Buffer.from(svg).toString("base64")}"></body></html>`;
  win.setContentSize(size, size);
  await win.loadURL(`data:text/html;base64,${Buffer.from(html).toString("base64")}`);
  // Let the image decode and the compositor produce a frame.
  await new Promise((r) => setTimeout(r, 150));
  const image = await win.webContents.capturePage({ x: 0, y: 0, width: size, height: size });
  return image.resize({ width: size, height: size }).toPNG();
}

// ICO container with PNG-compressed entries (supported since Windows Vista).
function makeIco(pngs) {
  const header = Buffer.alloc(6);
  header.writeUInt16LE(0, 0);            // reserved
  header.writeUInt16LE(1, 2);            // type: icon
  header.writeUInt16LE(pngs.length, 4);  // count
  const entries = [];
  let offset = 6 + 16 * pngs.length;
  for (const { size, png } of pngs) {
    const e = Buffer.alloc(16);
    e.writeUInt8(size >= 256 ? 0 : size, 0);   // width (0 = 256)
    e.writeUInt8(size >= 256 ? 0 : size, 1);   // height
    e.writeUInt8(0, 2);                        // palette
    e.writeUInt8(0, 3);                        // reserved
    e.writeUInt16LE(1, 4);                     // planes
    e.writeUInt16LE(32, 6);                    // bits per pixel
    e.writeUInt32LE(png.length, 8);
    e.writeUInt32LE(offset, 12);
    entries.push(e);
    offset += png.length;
  }
  return Buffer.concat([header, ...entries, ...pngs.map((p) => p.png)]);
}

app.whenReady().then(async () => {
  const svg = fs.readFileSync(svgPath, "utf8");
  const win = new BrowserWindow({
    show: false, transparent: true, frame: false, width: PNG_SIZE, height: PNG_SIZE,
    webPreferences: { offscreen: true, backgroundThrottling: false },
  });
  win.webContents.setBackgroundThrottling(false);
  win.setBackgroundColor("#00000000");

  const pngs = [];
  for (const size of ICO_SIZES) pngs.push({ size, png: await render(win, svg, size) });
  fs.writeFileSync(path.join(assets, "icon.ico"), makeIco(pngs));
  fs.writeFileSync(path.join(assets, "icon.png"), await render(win, svg, PNG_SIZE));
  console.log(`wrote ${path.join(assets, "icon.ico")} (${ICO_SIZES.join(", ")}) and icon.png (${PNG_SIZE})`);
  app.quit();
}).catch((e) => {
  console.error(e);
  app.exit(1);
});
