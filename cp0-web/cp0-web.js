// Cardenza CP0 机壳版驱动：复用 CardenzaWeb 的 wasm（cardenza.js/wasm + worklet 音频），
// 把引擎固定的 240x135 framebuffer 最近邻放大铺满 CP0 的 320x170 面板（与 P0 BuildPresented320 同口径），
// 电脑键盘按物理位置映射到 4x14 矩阵，IndexedDB 存档。C API 见 WebMain.cpp。
(function () {
  'use strict';

  const FB_W = 240, FB_H = 135;   // 引擎实际渲染尺寸
  const W = 320, H = 170;         // CP0 面板尺寸（放大目标）
  const COLUMNS = 14, ROWS = 4;
  const MOUNT = '/sdcard';
  const isMac = /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent);
  const statusEl = document.getElementById('status');
  const canvas = document.getElementById('screen');
  const ctx = canvas.getContext('2d', { alpha: false });

  let mod = null, booted = false, failed = false;
  let imageData = null, px32 = null, lastFrame = -1;

  function setStatus(t) { statusEl.textContent = t || ''; if (!t) statusEl.style.display = 'none'; }
  function say(t) { setStatus(t); }

  // 最近邻放大的源像素索引表（240->320, 135->170，铺满，与 P0 一致）
  const mapX = new Uint16Array(W), mapY = new Uint16Array(H);
  for (let x = 0; x < W; x++) mapX[x] = Math.min(FB_W - 1, (x * FB_W / W) | 0);
  for (let y = 0; y < H; y++) mapY[y] = Math.min(FB_H - 1, (y * FB_H / H) | 0);

  // RGB565 -> 0xAABBGGRR（与 CardenzaSimShot / cardenza-web.js 同扩展）
  const LUT = new Uint32Array(65536);
  for (let v = 0; v < 65536; v++) {
    const r = (((v >> 11) & 0x1f) * 255 / 31) | 0;
    const g = (((v >> 5) & 0x3f) * 255 / 63) | 0;
    const b = ((v & 0x1f) * 255 / 31) | 0;
    LUT[v] = (0xff000000 | (b << 16) | (g << 8) | r) >>> 0;
  }

  function present() {
    if (!booted) return;
    audio.onDeviceFrame();
    const frame = mod._cardenza_web_frame_counter();
    if (frame === lastFrame) return;
    lastFrame = frame;
    const fb = mod._cardenza_web_framebuffer() >> 1;
    const src = mod.HEAPU16;
    for (let y = 0; y < H; y++) {
      const row = fb + mapY[y] * FB_W, drow = y * W;
      for (let x = 0; x < W; x++) px32[drow + x] = LUT[src[row + mapX[x]]];
    }
    ctx.putImageData(imageData, 0, 0);
  }

  // ---- 存档（IndexedDB 挂 /sdcard，autoPersist）----
  function mountStorage(module) {
    const FS = module.FS;
    try { FS.mkdir(MOUNT); } catch (e) { /* exists */ }
    let persistent = true;
    try {
      if (typeof indexedDB === 'undefined') throw new Error('IndexedDB unavailable');
      FS.mount(module.IDBFS, { autoPersist: true }, MOUNT);
    } catch (e) { persistent = false; console.warn('CP0: no persistent storage', e); }
    if (!persistent) return;
    module.addRunDependency('cp0-storage');
    FS.syncfs(true, (err) => {
      if (err) console.warn('CP0: load storage failed', err);
      module.removeRunDependency('cp0-storage');
    });
  }
  function flushStorage() {
    if (booted && mod && typeof mod._cardenza_web_flush_storage === 'function') {
      try { mod._cardenza_web_flush_storage(); } catch (e) { /* ignore */ }
    }
    persistNow();
  }
  function persistNow() {
    if (!mod || !mod.FS) return;
    try { mod.FS.syncfs(false, (e) => { if (e) console.warn('CP0: sync failed', e); }); } catch (e) { /* ignore */ }
  }

  function start() {
    if (typeof window.createCardenzaModule !== 'function') { fail('cardenza.js 未定义 createCardenzaModule'); return; }
    say('加载 wasm…');
    window.createCardenzaModule({
      preRun: [mountStorage],
      print: (t) => console.log(t),
      printErr: (t) => console.warn(t),
      onAbort: (r) => fail('运行时停止：' + r),
      cardenzaBooted: () => onBooted(),
      cardenzaFailed: (m) => fail(m),
      cardenzaPresent: () => present(),
    }).then((m) => { mod = m; }).catch((e) => fail(String(e && e.message || e)));
  }

  function onBooted() {
    if (booted || failed) return;
    if (!mod) { setTimeout(onBooted, 0); return; }
    booted = true;
    releaseAll();
    imageData = ctx.createImageData(W, H);
    px32 = new Uint32Array(imageData.data.buffer);
    setStatus('');
    canvas.focus();
    present();
    audio.updateUi();
  }

  function fail(message) {
    failed = true;
    console.error('CP0:', message);
    statusEl.textContent = '错误：' + message;
    statusEl.style.color = '#e66';
  }

  // ---- 电脑键盘：物理位置 -> 4x14 矩阵（与 cardenza-web.js 相同）----
  const KEYMAP = new Map();
  const map = (code, row, col) => KEYMAP.set(code, [row, col]);
  map('Escape', 0, 0); map('Backquote', 0, 0);
  ['Digit1','Digit2','Digit3','Digit4','Digit5','Digit6','Digit7','Digit8','Digit9','Digit0','Minus','Equal']
    .forEach((c, i) => map(c, 0, i + 1));
  map('Backspace', 0, 13); map('Delete', 0, 13); map('NumpadAdd', 0, 13);
  map('Tab', 1, 0);
  ['KeyQ','KeyW','KeyE','KeyR','KeyT','KeyY','KeyU','KeyI','KeyO','KeyP','BracketLeft','BracketRight']
    .forEach((c, i) => map(c, 1, i + 1));
  map('Backslash', 1, 13); map('NumpadSubtract', 1, 13);
  map('CapsLock', 2, 0); map('ShiftLeft', 2, 0); map('ShiftRight', 2, 1);
  ['KeyA','KeyS','KeyD','KeyF','KeyG','KeyH','KeyJ','KeyK','KeyL','Semicolon','Quote']
    .forEach((c, i) => map(c, 2, i + 2));
  map('Enter', 2, 13); map('NumpadEnter', 2, 13);
  map('ControlLeft', 3, 0); map('ControlRight', 3, 0); map('AltLeft', 3, 1); map('AltRight', 3, 2);
  ['KeyZ','KeyX','KeyC','KeyV','KeyB','KeyN','KeyM','Comma','Period','Slash']
    .forEach((c, i) => map(c, 3, i + 3));
  map('Space', 3, 13);
  const HOME_CODE = 'Home';

  // ---- 屏上键热点：叠在官方机身 PNG 的 11×4 键位上，点击/触摸驱动 Cardenza 矩阵 ----
  // CP0 真机键盘是 11 列 ×4 行(46 键)，Cardenza 是 4×14。这里按 (row,col) 直连做个简化映射，
  // 完整精确操作走电脑键盘（46↔56 语义对齐是后续 P1 工作）。
  const keyboardEl = document.getElementById('hotspots');
  const keyEls = {};            // idx -> hotspot element
  const heldPtr = new Map();
  const KCOLS = 11, KROWS = 4;  // CP0 键盘 11×4
  const KX = 8, KY = 195, KW = 50, KH = 47, KCS = 57, KRS = 49; // 机身图 630×405 坐标（同 DesktopVirtualKeypad）
  const DW = 630, DH = 405;
  function buildHotspots() {
    for (let r = 0; r < KROWS; r++) for (let c = 0; c < KCOLS; c++) {
      const idx = r * COLUMNS + c;   // Cardenza 矩阵索引（c 0..10）
      const el = document.createElement('div');
      el.className = 'hot';
      el.style.left = ((KX + c * KCS) / DW * 100) + '%';
      el.style.top = ((KY + r * KRS) / DH * 100) + '%';
      el.style.width = (KW / DW * 100) + '%';
      el.style.height = (KH / DH * 100) + '%';
      el.dataset.idx = String(idx);
      keyEls[idx] = el;
      keyboardEl.appendChild(el);
    }
  }
  function bindPointerKeys() {
    keyboardEl.addEventListener('pointerdown', (e) => {
      const k = e.target.closest('.hot'); if (!k) return;
      e.preventDefault(); audio.userGesture();
      const idx = Number(k.dataset.idx);
      heldPtr.set(e.pointerId, idx);
      try { keyboardEl.setPointerCapture(e.pointerId); } catch (_) { /* ignore */ }
      setHeld(idx, 'ptr:' + e.pointerId, true);
    });
    const up = (e) => {
      const idx = heldPtr.get(e.pointerId); if (idx === undefined) return;
      heldPtr.delete(e.pointerId);
      setHeld(idx, 'ptr:' + e.pointerId, false);
    };
    keyboardEl.addEventListener('pointerup', up);
    keyboardEl.addEventListener('pointercancel', up);
    keyboardEl.addEventListener('lostpointercapture', up);
  }

  const holders = Array.from({ length: ROWS * COLUMNS }, () => new Set());
  const pcHeld = new Map();
  let homeSources = new Set();

  function setHeld(index, source, down) {
    const set = holders[index];
    const wasDown = set.size > 0;
    if (down) set.add(source); else set.delete(source);
    const isDown = set.size > 0;
    const el = keyEls[index]; if (el) el.classList.toggle('down', isDown);
    if (wasDown !== isDown && booted) mod._cardenza_web_set_key((index / COLUMNS) | 0, index % COLUMNS, isDown ? 1 : 0);
  }
  function setHome(source, down) {
    const wasDown = homeSources.size > 0;
    if (down) homeSources.add(source); else homeSources.delete(source);
    const isDown = homeSources.size > 0;
    if (wasDown !== isDown && booted) mod._cardenza_web_set_home(isDown ? 1 : 0);
  }
  function releaseAll() {
    for (let i = 0; i < holders.length; i++) if (holders[i].size) holders[i].clear();
    pcHeld.clear(); homeSources = new Set();
    if (booted) mod._cardenza_web_release_all();
  }

  function onKeyDown(e) {
    if (e.metaKey) { if (e.code === 'MetaLeft' || e.code === 'MetaRight') releaseAll(); return; }
    audio.userGesture();
    if (e.code === HOME_CODE) { e.preventDefault(); if (!e.repeat) setHome('pc:' + e.code, true); return; }
    const pos = KEYMAP.get(e.code);
    if (!pos) return;
    e.preventDefault();
    if (e.repeat) return;
    const index = pos[0] * COLUMNS + pos[1];
    pcHeld.set(e.code, index);
    setHeld(index, 'pc:' + e.code, true);
  }
  function onKeyUp(e) {
    if (e.code === HOME_CODE) { e.preventDefault(); setHome('pc:' + e.code, false); return; }
    const index = pcHeld.get(e.code);
    if (index === undefined) { if (KEYMAP.has(e.code)) e.preventDefault(); return; }
    e.preventDefault();
    pcHeld.delete(e.code);
    setHeld(index, 'pc:' + e.code, false);
  }

  function bindInput() {
    window.addEventListener('keydown', onKeyDown, true);
    window.addEventListener('keyup', onKeyUp, true);
    window.addEventListener('blur', () => releaseAll());
    document.addEventListener('visibilitychange', () => { if (document.hidden) { releaseAll(); flushStorage(); } });
    window.addEventListener('pagehide', flushStorage);
    for (const t of ['pointerdown', 'pointerup', 'touchend']) {
      document.addEventListener(t, () => audio.userGesture(), true);
    }
    window.addEventListener('resize', layout);
  }

  // 机身按窗口等比缩放由 CSS(min) 负责，这里无需计算
  function layout() { /* CSS handles sizing */ }

  // ---- 音频：主线程渲染（wasm 在这里），AudioWorklet 播放；无 worklet 时回落 ScriptProcessor ----
  const audio = (() => {
    const BASE_TARGET = 2048, MAX_TARGET = 6144, CHUNK = 256;
    let context = null, workletLoaded = false, node = null, mode = 'off', wanted = true;
    let sourceRate = 44100, posted = 0, consumed = 0, underruns = 0, target = BASE_TARGET;
    let lastUnderrunAt = 0, silentClock = 0, feedTimer = 0;
    let fifo = new Float32Array(16384), fifoStart = 0, fifoEnd = 0, fifoPosition = 0;
    const running = () => (mode === 'worklet' || mode === 'script') && context && context.state === 'running';
    function render(frames) {
      const pointer = mod._cardenza_web_render(frames) >> 2;
      return mod.HEAPF32.subarray(pointer, pointer + frames);
    }
    function feed() {
      if (!booted || mode !== 'worklet' || !running()) return;
      let buffered = posted - consumed, guard = 0;
      while (buffered < target && guard++ < 32) {
        const frames = Math.min(8192, Math.ceil((target - buffered) / CHUNK) * CHUNK);
        const samples = render(frames).slice();
        node.port.postMessage({ samples }, [samples.buffer]);
        posted += frames; buffered += frames;
      }
    }
    function renderSilently() {
      const now = performance.now();
      if (!silentClock) silentClock = now;
      let frames = Math.round((now - silentClock) * sourceRate / 1000);
      if (frames < CHUNK) return;
      silentClock += frames * 1000 / sourceRate;
      if (frames > 8192) frames = 8192;
      render(frames);
    }
    function onDeviceFrame() {
      if (running()) { silentClock = 0; if (mode === 'worklet') feed(); return; }
      renderSilently();
    }
    function onWorkletMessage(event) {
      const d = event.data;
      if (typeof d.consumed === 'number') consumed = d.consumed;
      if (typeof d.underruns === 'number' && d.underruns !== underruns) {
        underruns = d.underruns;
        const now = performance.now();
        if (now - lastUnderrunAt > 250 && target < MAX_TARGET) target += 512;
        lastUnderrunAt = now;
      } else if (target > BASE_TARGET && performance.now() - lastUnderrunAt > 30000) {
        target -= 256; lastUnderrunAt = performance.now();
      }
      feed();
    }
    function scriptProcess(event) {
      const output = event.outputBuffer.getChannelData(0);
      const step = sourceRate / context.sampleRate;
      const needed = Math.ceil(output.length * step) + 2;
      if (fifoEnd - fifoStart < needed) {
        fifo.copyWithin(0, fifoStart, fifoEnd); fifoEnd -= fifoStart; fifoPosition -= fifoStart; fifoStart = 0;
        const frames = Math.ceil((needed - fifoEnd) / CHUNK) * CHUNK + CHUNK;
        if (fifoEnd + frames > fifo.length) {
          const bigger = new Float32Array((fifoEnd + frames) * 2);
          bigger.set(fifo.subarray(0, fifoEnd)); fifo = bigger;
        }
        fifo.set(render(frames), fifoEnd); fifoEnd += frames;
      }
      let position = fifoPosition;
      for (let i = 0; i < output.length; i++) {
        const base = Math.floor(position), frac = position - base, a = fifo[base];
        output[i] = frac === 0 ? a : a + (fifo[base + 1] - a) * frac;
        position += step;
      }
      fifoPosition = position; fifoStart = Math.floor(position);
    }
    async function startOutput() {
      if (!booted || mode !== 'off') return;
      mode = 'starting';
      sourceRate = mod._cardenza_web_sample_rate();
      try {
        const Ctx = window.AudioContext || window.webkitAudioContext;
        if (!Ctx) throw new Error('无 Web Audio');
        if (!context) {
          try { context = new Ctx({ sampleRate: sourceRate, latencyHint: 'interactive' }); }
          catch (e) { context = new Ctx({ latencyHint: 'interactive' }); }
        }
        if (context.state !== 'running') context.resume().catch(() => {});
        posted = 0; consumed = 0; underruns = 0; target = BASE_TARGET;
        if (context.audioWorklet && typeof AudioWorkletNode === 'function') {
          if (!workletLoaded) { await context.audioWorklet.addModule('cardenza-audio-worklet.js'); workletLoaded = true; }
          node = new AudioWorkletNode(context, 'cardenza-output', {
            numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
            processorOptions: { sourceRate, prefillFrames: 1024, reportEveryQuanta: 2 },
          });
          node.port.onmessage = onWorkletMessage;
          node.connect(context.destination);
          mode = 'worklet'; feed();
          clearInterval(feedTimer); feedTimer = setInterval(feed, 10);
        } else {
          node = context.createScriptProcessor(2048, 0, 1);
          fifoStart = fifoEnd = fifoPosition = 0;
          node.onaudioprocess = scriptProcess;
          node.connect(context.destination); mode = 'script';
        }
        say('');
      } catch (e) {
        console.warn('CP0: 音频无法启动', e); mode = 'off';
        setStatus('音频失败：' + (e && e.message || e));
      }
      updateUi();
    }
    function stopOutput() {
      clearInterval(feedTimer);
      if (node) { try { node.disconnect(); } catch (e) {} if (node.port) node.port.onmessage = null; node.onaudioprocess = null; node = null; }
      if (context) context.suspend();
      mode = 'off'; updateUi();
    }
    function userGesture() {
      if (!booted || !wanted) return;
      if (mode === 'off') startOutput();
      else if (context && context.state === 'suspended') context.resume().catch(() => {});
    }
    function toggle() { if (mode === 'off') { wanted = true; startOutput(); } else { wanted = false; stopOutput(); } }
    function updateUi() {
      if (running()) document.documentElement.dataset.audio = 'on';
      else delete document.documentElement.dataset.audio;
    }
    return { onDeviceFrame, userGesture, toggle, updateUi };
  })();

  // ---- 启动 ----
  buildHotspots();
  bindPointerKeys();
  layout();
  bindInput();
  window.CardenzaCP0 = { start, fail, module: () => mod, persist: persistNow };
})();
