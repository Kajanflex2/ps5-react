// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Video Lab: plays every file in /app0/media and every URL listed in /app0/media/urls.txt
// ("Label | URL" per line), with the player's measurements on screen. media/ is not part of the
// repository: copy test footage there (on the PS5, into the title folder's media/).
// For unattended runs, /app0/dev/playlist.txt ("file seconds [seek-to]" per line) plays each file for that
// long, logs a summary line per file and quits.
import {useEffect, useMemo, useRef, useState} from 'react';
import {AppRegistry, View, Text, FocusScope, FileSystem, Video, BackHandler} from '@ps5-react/core';
import {useInterfaceSounds} from './sounds.js';

const MEDIA = '/app0/media';
const VIDEO_FILE = /\.(mp4|m4v|mov|mkv|webm|ts|m2ts|avi|flv|mpg|mpeg|ogv)$/i;

function readSources() {
  const sources = [];
  try {
    for (const entry of FileSystem.readDir(MEDIA)) {
      if (entry.isFile && VIDEO_FILE.test(entry.name))
        sources.push({label: entry.name, uri: `${MEDIA}/${entry.name}`, size: entry.size});
    }
  } catch {}
  try {
    for (const line of FileSystem.readFile(`${MEDIA}/urls.txt`).split('\n')) {
      const [label, uri] = line.includes('|') ? line.split('|').map(part => part.trim()) : [line.trim(), line.trim()];
      if (/^https?:\/\//.test(uri)) sources.push({label, uri});
    }
  } catch {}
  return sources.sort((a, b) => a.label.localeCompare(b.label));
}

const time = seconds => {
  const s = Math.max(0, Math.floor(seconds || 0));
  const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, rest = String(s % 60).padStart(2, '0');
  return h ? `${h}:${String(m).padStart(2, '0')}:${rest}` : `${m}:${rest}`;
};
const megabytes = bytes => (bytes ? `${(bytes / 1048576).toFixed(0)} MB` : 'stream');

function Library({sources, onOpen}) {
  return (
    <FocusScope autoFocus wrap>
      <View className="w-screen h-screen bg-ink px-24 py-16 gap-6">
        <Text className="font-inter text-title text-white">Video Lab</Text>
        <Text className="font-inter text-hint text-accent">
          {sources.length ? 'X: play    Circle: back' : `No media: put files in ${MEDIA} or URLs in ${MEDIA}/urls.txt`}
        </Text>
        <View className="gap-3">
          {sources.map(source => (
            <View key={source.uri} onPress={() => onOpen(source)} className="flex-row justify-between px-6 py-4 rounded-xl
              border-[3px] border-edge bg-panel focused:border-accent focused:bg-panel-focus">
              <Text className="font-inter text-body text-white">{source.label}</Text>
              <Text className="font-inter text-body text-accent">{megabytes(source.size)}</Text>
            </View>
          ))}
        </View>
      </View>
    </FocusScope>
  );
}

function Button({label, onPress}) {
  return (
    <View onPress={onPress} className="px-6 py-3 rounded-xl border-[3px] border-edge bg-panel
      focused:border-accent focused:bg-panel-focus">
      <Text className="font-inter text-body text-white">{label}</Text>
    </View>
  );
}

// Shown frames per second, from the player's counter.
function useRate(status) {
  const last = useRef({at: 0, shown: 0, rate: 0});
  const now = performance.now();
  if (status && now - last.current.at >= 1000) {
    last.current.rate = last.current.at ? (status.shownFrames - last.current.shown) * 1000 / (now - last.current.at) : 0;
    last.current.at = now;
    last.current.shown = status.shownFrames;
  }
  return last.current.rate;
}

function Player({source, onClose}) {
  const video = useRef(null);
  const [paused, setPaused] = useState(false);
  const [status, setStatus] = useState(null);
  const [info, setInfo] = useState(true);
  const [error, setError] = useState('');
  const [subtitle, setSubtitle] = useState('');
  const [subtitleTrack, setSubtitleTrack] = useState(-1);
  const rate = useRate(status);

  // The status arrives every frame; the overlay renders twice a second.
  useEffect(() => {
    const timer = setInterval(() => setStatus(video.current?.getStatus() ?? null), 500);
    return () => clearInterval(timer);
  }, []);

  const seekBy = seconds => {
    const current = video.current?.getStatus();
    if (current) video.current.seek(Math.max(0, Math.min(current.position + seconds, current.duration || Infinity)));
  };
  const nextTrack = () => {
    const current = video.current?.getStatus();
    if (!current?.audioTracks.length) return;
    const at = current.audioTracks.findIndex(track => track.index === current.audioTrack);
    video.current.selectAudioTrack(current.audioTracks[(at + 1) % current.audioTracks.length].index);
  };
  const nextSubtitles = () => {
    const tracks = video.current?.getStatus()?.subtitleTracks ?? [];
    const order = [-1, ...tracks.map(track => track.index)];
    setSubtitleTrack(current => order[(order.indexOf(current) + 1) % order.length]);
  };
  const onAction = action => {
    if (action === 'l1') seekBy(-60);
    else if (action === 'r1') seekBy(60);
    else if (action === 'triangle') setInfo(shown => !shown);
    else if (action === 'square') nextTrack();
    else return false;
    return true;
  };
  const track = status?.audioTracks.find(t => t.index === status.audioTrack);

  return (
    <FocusScope autoFocus trap onBack={() => { onClose(); return true; }} onAction={onAction}>
      <View className="w-screen h-screen bg-black">
        <Video ref={video} source={source.uri} paused={paused} className="absolute inset-0" subtitleTrack={subtitleTrack}
          onEnd={() => setPaused(true)} onError={event => setError(event.error)} onSubtitle={setSubtitle} />
        {!!subtitle && (
          <View className="absolute left-0 right-0 bottom-40 items-center">
            <View className="px-6 py-3 rounded-xl bg-black/60 items-center">
              {subtitle.split('\n').map((line, index) => (
                <Text key={index} className="font-inter text-title text-white">{line}</Text>
              ))}
            </View>
          </View>
        )}
        {info && (
          <View className="absolute left-12 top-12 px-8 py-6 rounded-2xl bg-ink/80 gap-2">
            <Text className="font-inter text-body text-white">{source.label}</Text>
            <Text className="font-inter text-hint text-accent">
              {status ? `${status.state}${status.seeking ? ' (seeking)' : ''} | ${time(status.position)} / ${time(status.duration)}`
                + ` | buffered ${time(status.buffered)}` : 'opening'}
            </Text>
            {status && (
              <Text className="font-inter text-hint text-white">
                {`${status.videoCodec} ${status.width}x${status.height} via ${status.decoder || '-'} | `
                  + `${rate.toFixed(1)} fps shown | ${status.droppedFrames} dropped | `
                  + `${status.audioClock ? 'audio' : 'wall'} clock, frames ${status.lateMs.toFixed(1)} ms late`}
              </Text>
            )}
            {status && (
              <Text className="font-inter text-hint text-white">
                {track ? `audio ${track.codec} ${track.channels}ch ${track.language || ''} `
                  + `(${status.audioTracks.length} tracks)` : 'no audio'}
              </Text>
            )}
            {!!error && <Text className="font-inter text-hint text-red-400">{error}</Text>}
          </View>
        )}
        <View className="absolute left-12 right-12 bottom-12 flex-row gap-4 items-center">
          <Button label="-10 s" onPress={() => seekBy(-10)} />
          <Button label={paused ? 'Play' : 'Pause'} onPress={() => setPaused(value => !value)} />
          <Button label="+10 s" onPress={() => seekBy(10)} />
          <Button label="Audio" onPress={nextTrack} />
          <Button label={subtitleTrack < 0 ? 'Subtitles off' : `Subtitles ${subtitleTrack}`} onPress={nextSubtitles} />
          <Text className="font-inter text-hint text-accent">L1/R1 seek 60 s | Triangle info | Square audio | Circle back</Text>
        </View>
      </View>
    </FocusScope>
  );
}

function readPlaylist(sources) {
  try {
    return FileSystem.readFile('/app0/dev/playlist.txt').split('\n').map(line => line.trim().split(/\s+/))
      .filter(([name]) => name).map(([name, seconds, seek]) => ({
        source: sources.find(source => source.label === name) ?? {label: name, uri: `${MEDIA}/${name}`},
        seconds: Number(seconds) || 10, seek: seek === undefined ? null : Number(seek)}));
  } catch {
    return null;
  }
}

// Plays the playlist through, one Player after another, logging what each measured.
function PlaylistRun({playlist}) {
  const [index, setIndex] = useState(0);
  const video = useRef(null);
  const item = playlist[index];
  useEffect(() => {
    if (!item) {
      console.log('playlist done');
      BackHandler.exitApp();
      return undefined;
    }
    const seek = item.seek === null ? null : setTimeout(() => video.current?.seek(item.seek), item.seconds * 500);
    const timer = setTimeout(() => {
      const s = video.current?.getStatus();
      console.log(s ? `playlist ${item.source.label}: ${s.state} ${s.decoder} ${s.videoCodec} ${s.width}x${s.height} `
        + `pos=${s.position.toFixed(1)} shown=${s.shownFrames} dropped=${s.droppedFrames} late=${s.lateMs.toFixed(1)}ms `
        + `clock=${s.audioClock ? 'audio' : 'wall'} audio=${s.audioCodec || '-'} ${s.error}` : `playlist ${item.source.label}: no status`);
      setIndex(i => i + 1);
    }, item.seconds * 1000);
    return () => { clearTimeout(timer); if (seek) clearTimeout(seek); };
  }, [index]);
  if (!item) return null;
  return (
    <View className="w-screen h-screen bg-black">
      <Video key={index} ref={video} source={item.source.uri} className="absolute inset-0"
        onError={event => console.log(`playlist ${item.source.label}: error ${event.error}`)} />
    </View>
  );
}

function App() {
  useInterfaceSounds();
  const sources = useMemo(readSources, []);
  const playlist = useMemo(() => readPlaylist(sources), [sources]);
  const [playing, setPlaying] = useState(null);
  useEffect(() => {
    const listener = BackHandler.addEventListener('hardwareBackPress', () => {
      if (!playing) return false;
      setPlaying(null);
      return true;
    });
    return () => listener.remove();
  }, [playing]);
  if (playlist) return <PlaylistRun playlist={playlist} />;
  return playing ? <Player source={playing} onClose={() => setPlaying(null)} />
    : <Library sources={sources} onOpen={setPlaying} />;
}

AppRegistry.registerComponent('video-lab', () => App);
