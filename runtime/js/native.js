// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// React Native-style modules over the host contract in native/shared/host_api.hpp.
// Every call is synchronous and runs on the render thread; see docs/NATIVE-API.md.
import {useEffect, useState} from 'react';
import {normalizeDownloadManifest} from './download-formats.js';
import {onFrameUnbatched} from './frame.js';

function host() {
  const api = globalThis.__ps5ReactNative;
  if (!api) {
    throw new Error('Native modules need a PS5 React host (globalThis.__ps5ReactNative is missing); '
      + 'run the app with npm run dev, npm run preview, or a PS5 build');
  }
  return api;
}

/**
 * @typedef {{name: string, isDirectory: boolean, isFile: boolean, size: number, modified: number}} FileStat
 *   `modified` is milliseconds since the Unix epoch.
 * @typedef {{device: string, path: string, type: string}} Mount
 * @typedef {{id: number, name: string}} User
 * @typedef {{model: string | null, firmware: string | null, cpuTemperature: number | null,
 *   socTemperature: number | null, cpuFrequency: number | null, freeMemory: number | null,
 *   processTime: number | null, language: string | null}} Device
 *   Temperatures in °C, frequency in Hz, memory in bytes, process time in µs; language is the
 *   system language as a BCP 47 tag ('en-US', 'pt-BR', 'zh-Hans').
 * @typedef {'up' | 'down' | 'left' | 'right' | 'cross' | 'circle' | 'triangle' | 'square' | 'l1' | 'r1'
 *   | 'l2' | 'r2' | 'l3' | 'r3' | 'options' | 'touchpad'} Button
 * @typedef {{connected: boolean, leftX: number, leftY: number, rightX: number, rightY: number,
 *   l2: number, r2: number, buttons: Button[]}} GamepadState
 *   Sticks are -1..1 with the deadzone applied; triggers are 0..1.
 */

export const Platform = {
  /** @returns {'ps5' | 'desktop'} */
  get OS() { return host().platform; },
  /**
   * @template T
   * @param {{ps5?: T, desktop?: T, default?: T}} spec
   * @returns {T | undefined}
   */
  select(spec) {
    const os = host().platform;
    return os in spec ? spec[os] : spec.default;
  },
};

export const DeviceInfo = {
  /** @returns {Device} */
  get: () => host().device.info(),
};

// The title is sandboxed: paths are app paths such as /app0 (read-only) and /download0.
export const FileSystem = {
  appDir: '/app0',
  dataDir: '/download0',
  tempDir: '/temp0',
  /** @param {string} path @returns {FileStat[]} */
  readDir: path => host().fs.readDir(path),
  /** @param {string} path @returns {FileStat | null} */
  // device/inode remain strings to preserve native 64-bit filesystem identities.
  stat: path => host().fs.stat(path),
  /** @param {string} path @returns {boolean} */
  exists: path => host().fs.stat(path) !== null,
  /** UTF-8, at most 8 MiB. @param {string} path @returns {string} */
  readFile: path => host().fs.readFile(path),
  /** @param {string} path @param {string} text */
  writeFile: (path, text) => host().fs.writeFile(path, text, false),
  /** @param {string} path @param {string} text */
  appendFile: (path, text) => host().fs.writeFile(path, text, true),
  /** @param {string} path @param {{recursive?: boolean}} [options] */
  mkdir: (path, {recursive = false} = {}) => host().fs.mkdir(path, recursive),
  /** Removes a file or an empty directory. @param {string} path */
  remove: path => host().fs.remove(path),
  /** Removes a file, or a directory with everything in it, without following symbolic links.
   * Synchronous: meant for small app-owned trees. @param {string} path */
  removeTree: path => host().fs.removeTree(path),
  /** @param {string} from @param {string} to */
  rename: (from, to) => host().fs.rename(from, to),
  /** Sets a file's or directory's permission bits, such as 0o777. */
  chmod: (path, mode) => host().fs.chmod(path, mode),
  /** Mounted filesystems visible to the host process. Console access requires
   * filesystemAccess: "console" in app.json. @returns {Mount[]} */
  mounts: () => host().fs.mounts(),
  /** Filesystem bytes; free is available to the app, clamped to 0..total.
   * @param {string} path @returns {{total: number, free: number}} */
  diskUsage: path => host().fs.diskUsage(path),
};

export const Notifications = {
  /** @param {string} message @param {string} [subMessage] @returns {boolean} */
  show: (message, subMessage = '') => host().notify(message, subMessage),
};

export const Users = {
  /** @returns {User | null} */
  getForeground: () => host().users.foreground(),
  /** @returns {User[]} */
  getLoggedIn: () => host().users.loggedIn(),
};

function rgb(color) {
  if (typeof color !== 'string') return color;
  const match = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(color);
  if (!match) throw new Error(`Controller.setLightBar: expected '#rrggbb' or {r, g, b}, got '${color}'`);
  const [r, g, b] = match.slice(1).map(hex => parseInt(hex, 16));
  return {r, g, b};
}

export const Controller = {
  /** @param {string | {r: number, g: number, b: number}} color '#rrggbb' or 0..255 channels */
  setLightBar(color) {
    const {r, g, b} = rgb(color);
    host().pad.setLightBar(r, g, b);
  },
  resetLightBar: () => host().pad.resetLightBar(),
  /** @param {number} [strength] 0..1 @param {number} [ms] */
  vibrate: (strength = 1, ms = 200) => host().pad.vibrate(strength, ms / 1000),
  /**
   * DualSense adaptive trigger resistance. From `from` of the travel (0 top, 1 bottom) on, the
   * trigger pushes back at `strength` (0..1, in eight steps); `strength` 0 releases it. The PS5
   * releases the triggers when the app quits; the desktop preview only logs the request.
   * @param {'l2' | 'r2' | 'both'} trigger @param {number} strength @param {number} [from]
   */
  setTriggerResistance(trigger, strength, from = 0) {
    const mask = TRIGGER_MASKS[trigger];
    if (!mask) throw new Error(`Controller.setTriggerResistance: trigger must be 'l2', 'r2' or 'both', got '${trigger}'`);
    if (!(strength >= 0 && strength <= 1) || !(from >= 0 && from <= 1))
      throw new RangeError(`Controller.setTriggerResistance: strength and from must be 0..1, got ${strength} and ${from}`);
    host().pad.setTriggerResistance(mask, Math.round(from * 9), strength > 0 ? Math.max(1, Math.round(strength * 8)) : 0);
  },
  /** @returns {GamepadState} */
  getState: () => host().pad.state(),
};

const TRIGGER_MASKS = {l2: 1, r2: 2, both: 3};

function sameState(a, b) {
  return a.connected === b.connected && a.leftX === b.leftX && a.leftY === b.leftY
    && a.rightX === b.rightX && a.rightY === b.rightY && a.l2 === b.l2 && a.r2 === b.r2
    && a.buttons.join() === b.buttons.join();
}

/**
 * Polls the controller while mounted and re-renders only when the state changes.
 * @param {number} [intervalMs]
 * @returns {GamepadState}
 */
export function useGamepad(intervalMs = 16) {
  const [state, setState] = useState(Controller.getState);
  useEffect(() => {
    const id = setInterval(() => {
      const next = Controller.getState();
      setState(previous => (sameState(previous, next) ? previous : next));
    }, intervalMs);
    return () => clearInterval(id);
  }, [intervalMs]);
  return state;
}

/**
 * Short interface sounds. Import a 16-bit PCM WAV (`import tick from './tick.wav'`) and the build
 * bakes it into the app; the import is the sound's name. Mixing runs on an audio thread.
 */
export const Sound = {
  /**
   * Starts a sound and returns at once. A sound plays one voice at a time: while it still sounds,
   * playing it again is skipped (so a held D-pad keeps ticking without stacking).
   * @param {string} name @param {{volume?: number, pan?: number}} [options] volume 0..1, pan -1..1
   * @returns {boolean} whether it started; false without an audio output
   */
  play: (name, {volume = 1, pan = 0} = {}) => host().sound.play(name, volume, pan),
  /** Master volume for every sound, 0..1 (0 mutes). @param {number} volume */
  setVolume: volume => host().sound.setVolume(volume),
};

export const Linking = {
  /** Opens an http(s) URL in the system browser. @param {string} url @returns {boolean} */
  openURL(url) {
    if (!/^https?:\/\//i.test(url)) throw new Error(`Linking.openURL: only http and https URLs are supported, got '${url}'`);
    return host().openURL(url);
  },
};

export const Power = {
  /**
   * While on, the console does not enter rest mode for inactivity (the desktop display does not
   * sleep): turn it on for a long download, off when it ends. The last call wins.
   * @param {boolean} enabled
   */
  keepAwake: enabled => host().power.keepAwake(Boolean(enabled)),
};

export const Fonts = {
  /**
   * The interface language, as a BCP 47 tag ('ja', 'zh-Hans', 'ar'), for text drawn from the
   * runtime fonts (docs/TEXT.md): it picks Japanese or Chinese forms of Han characters and the
   * shaping language. Defaults to the system language; set it before text renders.
   * @param {string} tag
   */
  setLanguage: tag => host().text.setLanguage(String(tag)),
};

const backListeners = [];

export const BackHandler = {
  /**
   * Closes the app after the current frame, like React Native's BackHandler.exitApp. With `relaunch`,
   * the app opens again once closed: true when that is arranged; otherwise it only closes, and the
   * reason is logged.
   * @param {{relaunch?: boolean}} [options]
   * @returns {boolean}
   */
  exitApp({relaunch = false} = {}) {
    const result = host().exit(relaunch);
    if (!relaunch) return false;
    if (result !== true) console.log(`BackHandler.exitApp: not reopening: ${result}`);
    return result === true;
  },
  /**
   * Circle, after the innermost FocusScope.onBack; the last listener added runs first.
   * @param {'hardwareBackPress'} event
   * @param {() => boolean | void} listener returns true to consume Circle
   * @returns {{remove: () => void}}
   */
  addEventListener(event, listener) {
    if (event !== 'hardwareBackPress') {
      throw new Error(`BackHandler.addEventListener: only 'hardwareBackPress' is supported, got '${event}'`);
    }
    backListeners.push(listener);
    return {remove() {
      const index = backListeners.indexOf(listener);
      if (index !== -1) backListeners.splice(index, 1);
    }};
  },
};

/** Internal: offers Circle to the listeners, last added first. @returns {boolean} consumed */
export function dispatchBackPress() {
  for (const listener of [...backListeners].reverse()) if (listener() === true) return true;
  return false;
}

const networkTasks = new Map();
let networkTimer = null;

function stopNetworkTimer() {
  if (networkTasks.size === 0 && networkTimer !== null) {
    clearInterval(networkTimer);
    networkTimer = null;
  }
}

function taskError(task, snapshot) {
  const error = new Error(`${task.call}${task.destination ? ` ${task.destination}` : ''}: `
    + (snapshot.error || snapshot.state));
  if (snapshot.state === 'cancelled') error.name = 'AbortError';
  return error;
}

function pollNetwork() {
  let snapshots;
  try {
    snapshots = [];
    for (const backend of new Set([...networkTasks.values()].map(task => task.backend)))
      snapshots.push(...host()[backend].poll().map(snapshot => ({...snapshot, backend})));
  } catch (error) {
    for (const task of networkTasks.values()) {
      try { host()[task.backend].cancel(task.id); } catch {}
      task.listeners.clear();
      task.reject(error);
    }
    networkTasks.clear();
    stopNetworkTimer();
    return;
  }
  for (const snapshot of snapshots) {
    const key = `${snapshot.backend}:${snapshot.id}`;
    const task = networkTasks.get(key);
    if (!task) continue;
    const {body, backend, ...progress} = snapshot;
    task.snapshot = Object.freeze({...progress, destination: task.destination});
    for (const listener of [...task.listeners]) {
      try { listener(task.snapshot); }
      catch (error) { console.error('Download progress listener failed:', error); }
    }
    if (snapshot.state === 'completed' || snapshot.state === 'failed' || snapshot.state === 'cancelled') {
      networkTasks.delete(key);
      task.listeners.clear();
      if (snapshot.state === 'completed') task.resolve({...snapshot, destination: task.destination});
      else task.reject(taskError(task, snapshot));
    }
  }
  stopNetworkTimer();
}

function networkTask(id, call, destination = '', backend = 'network') {
  const key = `${backend}:${id}`;
  const task = {id, backend, call, destination, listeners: new Set(),
    snapshot: Object.freeze({id, destination, state: 'queued', received: 0, written: 0, total: null,
      bytesPerSecond: 0, connections: 0, retries: 0, bufferedBytes: 0})};
  const done = new Promise((resolve, reject) => { task.resolve = resolve; task.reject = reject; });
  // A task can be observed through subscriptions without awaiting its completion.
  done.catch(() => {});
  networkTasks.set(key, task);
  if (networkTimer === null) networkTimer = setInterval(pollNetwork, 250);
  return Object.freeze({
    id, done,
    cancel() { if (networkTasks.has(key)) host()[backend].cancel(id); },
    get snapshot() { return task.snapshot; },
    subscribe(listener) {
      if (typeof listener !== 'function') throw new TypeError('Downloads.subscribe: expected a function');
      listener(task.snapshot);
      if (networkTasks.has(key)) task.listeners.add(listener);
      return () => task.listeners.delete(listener);
    },
  });
}

/** Native HTTP subset for bounded text/JSON responses; does not require a browser. */
export const Http = {
  async request(url, options = {}) {
    const signal = options.signal;
    if (signal && (typeof signal.addEventListener !== 'function' || typeof signal.removeEventListener !== 'function')) {
      throw new TypeError('Http.request: signal must support abort event listeners');
    }
    const id = host().network.request(url, options);
    const task = networkTask(id, 'Http.request');
    const abort = () => task.cancel();
    if (signal) {
      if (signal.aborted) abort();
      else signal.addEventListener('abort', abort, {once: true});
    }
    try {
      const response = await task.done;
      return Object.freeze({status: response.status, ok: response.status >= 200 && response.status < 300,
        url: response.url || url, headers: Object.freeze({...response.headers}),
        text: async () => response.body, json: async () => JSON.parse(response.body)});
    } finally {
      if (signal) signal.removeEventListener('abort', abort);
    }
  },
};

/** Large binary files stay native; only progress and the completion result reach JavaScript. */
export const Downloads = {
  // recoverCompleted verifies a native completion receipt and the final file's SHA-256.
  // It never accepts an existing destination based on size or filename alone.
  enqueue({url, destination, ...options}) {
    const id = host().network.download(url ?? options.pieces?.[0]?.url, destination, options);
    return networkTask(id, 'Downloads.enqueue', destination);
  },
  enqueueManifest({manifest, destination, ...options}) {
    return Downloads.enqueue({...options, ...normalizeDownloadManifest(manifest), destination});
  },
  get transportVersion() { return host().network.version(); },
};

/** Streaming archive extraction off-thread; source volumes are preserved on failure/cancel. */
export const Archives = Object.freeze({
  extract({sources, destination, maxBytes = 1024**4, password}) {
    if (password != null && (typeof password !== 'string' || password.length > 1024 || password.includes('\0')))
      throw new TypeError('Archives.extract: password must be a string without NUL, at most 1024 UTF-8 bytes.');
    const id = host().archives.extract(sources, destination, maxBytes, password ?? '');
    return networkTask(id, 'Archives.extract', destination, 'archives');
  },
  /** What the files' leading bytes are, and why their headers so far already rule out extraction.
   * Reads a few headers synchronously; also works on a download's growing `.part` file.
   * @param {string[]} sources ordered volumes @returns {{kind: string, refusal: string}} */
  inspect: sources => host().archives.inspect(sources),
});

/**
 * PKG installation through the console's install service (PS5 only; it needs a payload loader on port
 * 9021). The task's snapshot carries `written`/`total` bytes, the console's install `status` and the
 * package's `contentId`; it completes once the title is playable. Cancelling stops reporting only.
 */
export const Packages = Object.freeze({
  install({path, name = ''}) {
    const id = host().packages.install(path, name);
    return networkTask(id, 'Packages.install', path, 'packages');
  },
});

/**
 * UDP broadcast on the local network, for finding services that answer one (Jellyfin and Emby on
 * 7359, for instance). Replies come from anyone on the network: check what they claim before use.
 */
export const LocalNetwork = Object.freeze({
  /**
   * Sends `message` to every host on the network at `port` and gathers the replies for `timeoutMs`
   * (100–10000) or until `maxReplies` (1–64) arrive.
   * @param {{port: number, message: string, timeoutMs?: number, maxReplies?: number,
   *   signal?: {aborted: boolean, addEventListener: Function, removeEventListener: Function}}} options
   * @returns {Promise<ReadonlyArray<{address: string, port: number, text: string}>>} `address` is the sender's IPv4
   */
  async broadcast({port, message, timeoutMs = 1500, maxReplies = 16, signal}) {
    if (signal && (typeof signal.addEventListener !== 'function' || typeof signal.removeEventListener !== 'function')) {
      throw new TypeError('LocalNetwork.broadcast: signal must support abort event listeners');
    }
    const id = host().lan.broadcast(port, message, timeoutMs, maxReplies);
    const task = networkTask(id, `LocalNetwork.broadcast ${port}`, '', 'lan');
    const abort = () => task.cancel();
    if (signal) {
      if (signal.aborted) abort();
      else signal.addEventListener('abort', abort, {once: true});
    }
    try {
      const {replies} = await task.done;
      return Object.freeze(replies.map(reply => Object.freeze({address: reply.address, port: reply.port, text: reply.body})));
    } finally {
      if (signal) signal.removeEventListener('abort', abort);
    }
  },
});

/** Read-only, filename-bound Vikingfile capture on PS5; no browser data is retained. */
export const BrowserCapture = Object.freeze({
  capture({prefix = 'https://vikingfile.com/d/', suffix, timeoutSeconds = 180}) {
    const id = host().browser.capture(prefix, suffix, timeoutSeconds);
    return networkTask(id, 'BrowserCapture.capture', '', 'browser');
  },
});

const pendingImages = new Map();
// Finished loads the host has handed over and no listener has received yet, by id in arrival order.
const arrivedImages = new Map();
let stopImagePolling = null;

// Each image listener runs outside the frame batch, so its LegacyRoot render commits before elapsed
// time is checked. A single synchronous render cannot be preempted, but delivery stops after the
// first listener that carries the frame past the budget.
const IMAGE_DELIVERY_BUDGET_MS = 4;

function pollImages() {
  for (const result of host().image.poll()) {
    if (pendingImages.has(result.id)) arrivedImages.set(result.id, result);
  }
  const start = performance.now();
  for (const [id, result] of arrivedImages) {
    const listeners = pendingImages.get(id);
    if (!listeners?.size) {
      arrivedImages.delete(id);
      continue;
    }
    for (const listener of listeners) {
      listeners.delete(listener);
      listener(result);
      if (listeners.size === 0) {
        pendingImages.delete(id);
        arrivedImages.delete(id);
      }
      if (performance.now() - start >= IMAGE_DELIVERY_BUDGET_MS) break;
    }
    if (performance.now() - start >= IMAGE_DELIVERY_BUDGET_MS) break;
  }
  if (pendingImages.size === 0 && arrivedImages.size === 0) {
    stopImagePolling();
    stopImagePolling = null;
  }
}

// One more poll: a release can push unused images over the cache budget, and polls evict.
function pollImagesSoon() {
  stopImagePolling ??= onFrameUnbatched(pollImages);
}

/**
 * Internal: the native reference behind one `<Image source={{uri}}>`. `listener` receives
 * `{state, name, width, height, error}` now and, while loading, once more when the load finishes;
 * the returned function releases the reference, cancelling the load when nothing else holds it.
 * @param {string} uri @param {number} width @param {number} height box in render pixels
 * @param {number} fit 0 cover, 1 contain, 2 stretch, 3 none
 * @param {boolean} prefetch loads after images that elements draw
 * @param {(result: {state: string, name: string, width: number, height: number, error: string, color: string | null}) => void} listener
 * @returns {() => void}
 */
/**
 * Internal: replaces the list of URLs the host fetches into its disk cache in the background, while
 * no image load waits. @param {string[]} uris
 */
export function warmImages(uris) {
  host().image.warm(uris);
}

/**
 * Internal: holds image requests to the network back for `ms`, called every frame a ScrollView
 * moves, so a held key does not fetch the art of every row it passes. @param {number} ms
 */
export function deferImageFetches(ms) {
  host().image.defer(ms);
}

export function acquireImage(uri, width, height, fit, prefetch, listener) {
  const result = host().image.load(uri, width, height, fit, prefetch);
  const {id} = result;
  if (result.state === 'loading') {
    if (!pendingImages.has(id)) pendingImages.set(id, new Set());
    pendingImages.get(id).add(listener);
    pollImagesSoon();
  }
  const release = () => {
    const listeners = pendingImages.get(id);
    listeners?.delete(listener);
    if (listeners?.size === 0) {
      pendingImages.delete(id);
      arrivedImages.delete(id);
    }
    host().image.release(id, prefetch);
    pollImagesSoon();
  };
  try {
    listener(result);
  } catch (error) {
    release();
    throw error;
  }
  return release;
}
