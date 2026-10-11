// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
const listeners = new Set();

const pending = [];

function deliver() {
  for (const action of pending.splice(0)) for (const listener of listeners) listener(action);
}

// Input protocol v4 (versioned apart from the native API's ABI in host_api.hpp): hosts emit
// up/down/left/right/confirm/back/l1/r1/l2/r2/triangle/square (v2: the first six; v3: up to square),
// and options for apps with "optionsButton": "app"
// on the JS/render thread, before the frame's pump. Actions are delivered from the pump's microtask
// drain, which runs inside React's batch like the bridge's own events: a direct host call is outside
// it, so each setState of a handler would render and commit on its own, and the frame log would
// count that render outside `react`.
globalThis.__ps5ReactDispatch = action => {
  if (pending.push(action) === 1) Promise.resolve().then(deliver);
};

/**
 * The host actions, as native/shared/actions.hpp names them.
 * @typedef {'l1' | 'r1' | 'l2' | 'r2' | 'triangle' | 'square' | 'options'} ButtonAction shoulders, triggers, Triangle,
 *   Square, and Options where the app takes it ("optionsButton": "app")
 * @typedef {'up' | 'down' | 'left' | 'right' | 'confirm' | 'back' | ButtonAction} Action
 */

/**
 * Internal: receive every host action until the returned function is called.
 * @param {(action: Action) => void} listener
 */
export function subscribeInput(listener) {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}
