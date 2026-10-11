// Rasterize the installed Lucide package for RmlUi's bitmap-only texture loader.
const fs = require("node:fs/promises");
const path = require("node:path");
const packageRoot = process.argv[2];
if (!packageRoot) throw new Error("Pass the directory containing sharp and lucide.");
const sharp = require(path.join(packageRoot, "sharp"));
const lucide = require(path.join(packageRoot, "lucide"));
const output = path.resolve(__dirname, "../../ui/icons");

async function icon(name, file, size = 32, color = "#d3d3d8", fill = "none") {
  const nodes = lucide.icons[name];
  if (!nodes) throw new Error(`Missing Lucide icon: ${name}`);
  const body = nodes.map(([tag, attrs]) =>
    `<${tag} ${Object.entries(attrs).map(([k, v]) => `${k}="${v}"`).join(" ")}/>`
  ).join("");
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="${size}" height="${size}" fill="${fill}" stroke="${color}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">${body}</svg>`;
  await sharp(Buffer.from(svg)).png().toFile(path.join(output, file));
}

async function materialTypeIcon() {
  const size = 128;
  const pixels = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const nx = (x + 0.5 - 64) / 45;
      const ny = (64 - y - 0.5) / 45;
      const r2 = nx * nx + ny * ny;
      const offset = (y * size + x) * 4;
      let shade = ((x >> 4) + (y >> 4)) % 2 ? 23 : 26;
      if (r2 < 1) {
        const nz = Math.sqrt(1 - r2);
        const diffuse = Math.max(0, -nx * 0.43 + ny * 0.58 + nz * 0.69);
        const specular = Math.pow(Math.max(0, -nx * 0.25 + ny * 0.34 + nz * 0.907), 40);
        shade = Math.min(242, 52 + 131 * diffuse + 72 * specular);
      } else if (y > 103 && y < 117) {
        const shadow = Math.exp(-(((x - 64) / 37) ** 2 + ((y - 108) / 5) ** 2));
        shade *= 1 - shadow * 0.65;
      }
      pixels[offset] = shade;
      pixels[offset + 1] = shade;
      pixels[offset + 2] = shade + (r2 < 1 ? 2 : 0);
      pixels[offset + 3] = 255;
    }
  }
  // A neutral type placeholder, not an asset-specific or GPU material preview.
  await sharp(pixels, { raw: { width: size, height: size, channels: 4 } })
    .png().toFile(path.join(output, "material-instance.png"));
}

async function main() {
  await fs.mkdir(output, { recursive: true });
  for (const [name, file] of [
    ["ArrowLeft", "arrow-left.png"], ["ArrowRight", "arrow-right.png"],
    ["ArrowUp", "arrow-up.png"], ["PanelLeft", "panel-left.png"],
    ["LayoutGrid", "layout-grid.png"], ["List", "list.png"],
    ["Search", "search.png"], ["ChevronRight", "chevron-right.png"],
    ["FileBox", "file-box.png"], ["Check", "check.png"], ["RefreshCw", "refresh.png"]
  ]) await icon(name, file);
  await icon("Folder", "folder.png", 32, "#d6b269", "#7d682f");
  await icon("Folder", "folder-tile.png", 128, "#d6b269", "#7d682f");
  await icon("Cuboid", "mesh.png", 128, "#74c2d1", "#233b40");
  await icon("Map", "scene.png", 128, "#81aee9", "#293a52");
  await icon("Image", "texture.png", 128, "#d38ba8", "#442f38");
  await icon("Mountain", "terrain.png", 128, "#cfb875", "#403b29");
  // Scene Outliner actor type icons: one per World actor type, 32px like the toolbar icons.
  await icon("Sun", "light-directional.png", 32, "#e5c17b");
  await icon("Lightbulb", "light-point.png", 32, "#e5c17b");
  await icon("Flashlight", "light-spot.png", 32, "#e5c17b");
  await icon("Square", "light-area.png", 32, "#e5c17b");
  await icon("Camera", "camera.png", 32, "#b79ae6");
  await icon("CloudSun", "environment.png", 32, "#81aee9");
  await icon("Wind", "wind.png", 32, "#9fd0a8");
  await materialTypeIcon();
  await fs.copyFile(path.join(packageRoot, "lucide/LICENSE"),
    path.join(output, "LUCIDE-LICENSE.txt"));
}
main().catch(error => { console.error(error); process.exitCode = 1; });
