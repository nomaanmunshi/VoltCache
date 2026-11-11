const { chromium } = require("playwright");
const { spawn } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");
const root = path.resolve(__dirname, "..");
const work = fs.mkdtempSync(path.join(os.tmpdir(), "voltcache-ui-"));
const processes = [];
function start(command, args, pattern) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { cwd: root });
    processes.push(child);
    let output = "";
    const deadline = setTimeout(
      () => reject(new Error("Service startup timed out: " + output)),
      8000,
    );
    child.stdout.on("data", (bytes) => {
      output += bytes.toString();
      const found = output.match(pattern);
      if (found) {
        clearTimeout(deadline);
        resolve({ child, port: Number(found[1]) });
      }
    });
    child.stderr.on("data", (data) => {
      output += data.toString();
    });
    child.on("exit", (code) => {
      clearTimeout(deadline);
      if (code) reject(new Error(output));
    });
  });
}
(async () => {
  let browser;
  try {
    const cache = await start(
      root + "/build/voltcache-server",
      ["--port", "0"],
      /listening on 127\.0\.0\.1:(\d+)/,
    );
    const demo = await start(
      "python3",
      [
        "demo/app.py",
        "--port",
        "0",
        "--cache-port",
        String(cache.port),
        "--database",
        work + "/links.sqlite3",
      ],
      /URL shortener: http:\/\/127\.0\.0\.1:(\d+)/,
    );
    browser = await chromium.launch({
      executablePath: process.env.CHROMIUM_EXECUTABLE || undefined,
      headless: true,
      args: ["--no-sandbox", "--no-zygote", "--single-process", "--disable-gpu"],
    });
    const page = await browser.newPage({
      viewport: { width: 1440, height: 1080 },
    });
    const errors = [];
    page.on("pageerror", (error) => errors.push(error.message));
    await page.goto("http://127.0.0.1:" + demo.port);
    await page
      .locator("#status-text")
      .filter({ hasText: "Cache connected" })
      .waitFor();
    await page.locator("#destination").fill("https://example.com/hello");
    await page.locator("#alias").fill("demo-link");
    await page.locator("#create-button").click();
    await page.locator("#result").waitFor({ state: "visible" });
    await page.locator("#resolve-button").click();
    await page
      .locator("#trace-title")
      .filter({ hasText: "Database lookup" })
      .waitFor();
    await page.locator("#resolve-button").click();
    await page
      .locator("#trace-title")
      .filter({ hasText: "Cache hit" })
      .waitFor();
    await page.screenshot({
      path: path.join(work, "desktop.png"),
      fullPage: true,
      animations: "disabled",
    });
    await page.locator("#cache-hits").filter({ hasText: /^1$/ }).waitFor();
    for (const width of [390, 700, 1024]) {
      await page.setViewportSize({ width, height: 900 });
      const overflow = await page.evaluate(
        () => document.documentElement.scrollWidth > window.innerWidth,
      );
      if (overflow) throw new Error("Page overflows at width " + width);
      await page.screenshot({
        path: path.join(work, `viewport-${width}.png`),
        fullPage: true,
        animations: "disabled",
      });
    }
    await page.emulateMedia({ reducedMotion: "reduce" });
    const motion = await page
      .locator(".layer-top")
      .evaluate((el) => getComputedStyle(el).animationName);
    if (motion !== "none") throw new Error("Reduced motion ignored");
    cache.child.kill("SIGTERM");
    await new Promise((resolve) => cache.child.once("exit", resolve));
    await page.locator("#resolve-button").click();
    await page
      .locator("#trace-title")
      .filter({ hasText: "Database lookup" })
      .waitFor();
    await page
      .locator("#status-text")
      .filter({ hasText: "SQLite fallback" })
      .waitFor();
    if (errors.length) throw new Error(errors.join("\n"));
    console.log(
      "Browser checks passed: create, miss, hit, fallback, live counters, three responsive widths, reduced motion, no JavaScript errors.",
    );
  } finally {
    if (browser) await browser.close();
    for (const process of processes.reverse()) process.kill("SIGTERM");
    fs.rmSync(work, { recursive: true, force: true });
  }
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
