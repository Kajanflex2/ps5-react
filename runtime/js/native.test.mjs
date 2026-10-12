// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import test from 'node:test';
import {acquireImage, Archives, BrowserCapture, Downloads, FileSystem, Http, Power, Sound} from './native.js';

test('native network tasks deliver progress, release listeners and stop polling', async () => {
  let tick;
  let clears = 0;
  let snapshots = [];
  const cancelled = [];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => { clears++; };
  globalThis.__ps5ReactNative = {network: {
    download: () => 1, request: () => 2,
    cancel: id => cancelled.push(id), poll: () => snapshots, version: () => 'fixture',
  }};
  try {
    const task = Downloads.enqueue({url: 'https://example.com/file', destination: '/download0/file'});
    const states = [];
    const unsubscribe = task.subscribe(progress => states.push(progress.state));
    snapshots = [{id: 1, state: 'downloading', body: 'must not reach progress', written: 12}];
    tick();
    assert.equal(task.snapshot.destination, '/download0/file');
    assert.equal('body' in task.snapshot, false);
    unsubscribe();
    snapshots = [{id: 1, state: 'completed', written: 24}];
    tick();
    assert.equal((await task.done).written, 24);
    assert.deepEqual(states, ['queued', 'downloading']);
    assert.equal(clears, 1);
    task.cancel();
    assert.deepEqual(cancelled, []);
    const aborted = Downloads.enqueue({url: 'https://example.com/file', destination: '/download0/file'});
    aborted.cancel();
    snapshots = [{id: 1, state: 'cancelled'}];
    tick();
    await assert.rejects(aborted.done, error => error.name === 'AbortError' && error.message.includes('/download0/file'));
    assert.deepEqual(cancelled, [1]);
    const response = Http.request('https://example.com/json');
    snapshots = [{id: 2, state: 'completed', status: 404, url: 'https://example.com/final', headers: {'content-type': 'application/json'}, body: '{"value":42}'}];
    tick();
    const result = await response;
    assert.equal(result.ok, false);
    assert.equal(result.status, 404);
    assert.equal(result.url, 'https://example.com/final');
    assert.equal(result.headers['content-type'], 'application/json');
    assert.equal(Object.isFrozen(result.headers), true);
    assert.deepEqual(await result.json(), {value: 42});
    assert.equal(await result.text(), '{"value":42}');
    assert.equal(clears, 3);
  } finally {
    globalThis.setInterval = originalInterval;
    globalThis.clearInterval = originalClear;
    delete globalThis.__ps5ReactNative;
  }
});

test('removeAsync resolves once the worker removed the path and rejects with the path and system error', async () => {
  let tick;
  let snapshots = [];
  const started = [];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps5ReactNative = {
    fs: {removeAsync: path => {
      if (path.includes('..')) throw new Error(`fs.removeAsync ${path}: Path is not allowed`);
      return started.push(path);
    }},
    removals: {poll: () => snapshots},
  };
  try {
    const removed = FileSystem.removeAsync('/download0/game');
    const refused = FileSystem.removeAsync('/download0/locked');
    assert.deepEqual(started, ['/download0/game', '/download0/locked']);
    snapshots = [{id: 1, state: 'removing', error: ''}, {id: 2, state: 'queued', error: ''}];
    tick();
    snapshots = [{id: 1, state: 'completed', error: ''}, {id: 2, state: 'failed', error: 'Permission denied'}];
    tick();
    assert.equal(await removed, undefined);
    await assert.rejects(refused, {message: 'fs.removeAsync /download0/locked: Permission denied'});
    await assert.rejects(FileSystem.removeAsync('/download0/../x'), /Path is not allowed/);
  } finally {
    globalThis.setInterval = originalInterval;
    globalThis.clearInterval = originalClear;
    delete globalThis.__ps5ReactNative;
  }
});

test('remote images poll only while loads are pending and release every reference', () => {
  const released = [];
  let finished = [];
  let polls = 0;
  globalThis.__ps5ReactNative = {image: {
    load: (uri, width, height) => (width === 10
      ? {id: 7, state: 'ready', name: '@image:7', width: 10, height: 10, error: ''}
      : {id: 8, state: 'loading', name: '', width: 0, height: 0, error: ''}),
    release: id => released.push(id),
    poll: () => { polls++; const out = finished; finished = []; return out; },
  }};
  const seen = [];
  const releaseReady = acquireImage('https://example.com/a.jpg', 10, 10, 0, false, result => seen.push(result.state));
  globalThis.__ps5ReactFrame(16);
  assert.equal(polls, 0, 'a cached image needs no polling');
  const releaseLoading = acquireImage('https://example.com/b.jpg', 20, 20, 0, false, result => seen.push(result.state));
  globalThis.__ps5ReactFrame(16);
  finished = [{id: 8, state: 'ready', name: '@image:8', width: 20, height: 20, error: ''}];
  globalThis.__ps5ReactFrame(16);
  globalThis.__ps5ReactFrame(16);
  assert.equal(polls, 2, 'polling stops once nothing is pending');
  assert.deepEqual(seen, ['ready', 'loading', 'ready']);
  releaseReady();
  releaseLoading();
  assert.deepEqual(released, [7, 8]);
  globalThis.__ps5ReactFrame(16);
  globalThis.__ps5ReactFrame(16);
  assert.equal(polls, 3, 'a release polls once more so eviction can run');
  delete globalThis.__ps5ReactNative;
});

test('Power.keepAwake passes a boolean to the host', () => {
  const calls = [];
  globalThis.__ps5ReactNative = {power: {keepAwake: enabled => calls.push(enabled)}};
  try {
    Power.keepAwake(true);
    Power.keepAwake(0);
    assert.deepEqual(calls, [true, false]);
  } finally {
    delete globalThis.__ps5ReactNative;
  }
});

test('Sound passes the name, volume and pan to the host and returns whether it started', () => {
  const calls = [];
  globalThis.__ps5ReactNative = {sound: {
    play: (name, volume, pan) => { calls.push(['play', name, volume, pan]); return name === 'tick'; },
    setVolume: volume => calls.push(['setVolume', volume]),
  }};
  try {
    assert.equal(Sound.play('tick'), true);
    assert.equal(Sound.play('page', {pan: -0.4}), false);
    Sound.play('tick', {volume: 0.5});
    Sound.setVolume(0);
    assert.deepEqual(calls, [['play', 'tick', 1, 0], ['play', 'page', 1, -0.4], ['play', 'tick', 0.5, 0],
      ['setVolume', 0]]);
  } finally {
    delete globalThis.__ps5ReactNative;
  }
});

test('archive and download task IDs are isolated while sharing the polling timer', async () => {
  const originalInterval = globalThis.setInterval, originalClear = globalThis.clearInterval;
  let tick, networkSnapshots = [], archiveSnapshots = [];
  const cancelled = [];
  const extractions = [];
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps5ReactNative = {
    network: {download: () => 1, poll: () => networkSnapshots, cancel: id => cancelled.push(`network:${id}`)},
    archives: {extract: (...args) => { extractions.push(args); return 1; }, poll: () => archiveSnapshots, cancel: id => cancelled.push(`archives:${id}`)},
  };
  try {
    const download = Downloads.enqueue({url: 'https://example.com/file', destination: '/download0/file'});
    const archive = Archives.extract({sources: ['/download0/file'], destination: '/download0/out', password: '[DLPSGAME.COM]'});
    assert.deepEqual(extractions, [[['/download0/file'], '/download0/out', 1024**4, '[DLPSGAME.COM]', false]]);
    assert.equal(Archives.streams, true);
    for (const password of [42, 'bad\0password', 'x'.repeat(1025)])
      assert.throws(() => Archives.extract({sources: ['/download0/file'], destination: '/download0/out', password}), /password/);
    assert.equal(extractions.length, 1);
    archive.cancel(); assert.deepEqual(cancelled, ['archives:1']);
    archiveSnapshots = [{id: 1, state: 'extracting', phase: 'finishing', written: 50, synced: 1, syncTotal: 3}]; tick();
    assert.deepEqual([archive.snapshot.phase, archive.snapshot.synced, archive.snapshot.syncTotal], ['finishing', 1, 3]);
    archiveSnapshots = [{id: 1, state: 'completed', written: 50, artifacts: ['game.ffpfsc']}]; tick();
    assert.deepEqual((await archive.done).artifacts, ['game.ffpfsc']);
    assert.equal(download.snapshot.state, 'queued');
    networkSnapshots = [{id: 1, state: 'completed', written: 100}]; tick();
    assert.equal((await download.done).written, 100);
  } finally {
    globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear;
    delete globalThis.__ps5ReactNative;
  }
});


test('browser capture delivers only its result and cancellation rejects as AbortError', async () => {
  const originalInterval = globalThis.setInterval, originalClear = globalThis.clearInterval;
  let tick, snapshots = [], next = 1;
  const calls = [], cancelled = [];
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps5ReactNative = {browser: {
    capture: (...args) => { calls.push(args); return next++; },
    poll: () => { const result = snapshots; snapshots = []; return result; },
    cancel: id => cancelled.push(id),
  }};
  try {
    const capture = BrowserCapture.capture({suffix: '/Game.pkg'});
    assert.deepEqual(calls, [['https://vikingfile.com/d/', '/Game.pkg', 180]]);
    const states = [];
    const off = capture.subscribe(snapshot => states.push(snapshot.state));
    snapshots = [{id: 1, state: 'capturing'}]; tick();
    const url = 'https://vikingfile.com/d/Ab01234567/Game.pkg';
    snapshots = [{id: 1, state: 'completed', url}]; tick();
    assert.equal((await capture.done).url, url);
    assert.deepEqual(states, ['queued', 'capturing', 'completed']); off();
    const second = BrowserCapture.capture({suffix: '/Other.pkg', timeoutSeconds: 30});
    second.cancel(); assert.deepEqual(cancelled, [2]);
    snapshots = [{id: 2, state: 'cancelled'}]; tick();
    await assert.rejects(second.done, {name: 'AbortError'});
  } finally {
    globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear;
    delete globalThis.__ps5ReactNative;
  }
});
