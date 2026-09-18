# MOQ Plugin

## Introduction

This is an OpenMOQ plugin for OBS that streams video over [MOQ](https://datatracker.ietf.org/wg/moq/about/), using [openmoq/moq5](https://github.com/openmoq/moq5) as the underlying protocol implementation.

**This project is under active development.** It may contain bugs, and several features are still in progress or not yet implemented. See the [Roadmap](#roadmap) section below for what's currently missing.

## Dependencies

* [libmoq](https://github.com/openmoq/moq5) (openmoq/moq5), MOQ protocol implementation, built with the `service` component. Must be built from the [qualabs/moq5](https://github.com/qualabs/moq5) fork, whose codec-signaling work is proposed upstream as [PR #5](https://github.com/openmoq/moq5/pull/5) (see [Required libmoq fork](#required-libmoq-fork) below).
* [OBS Studio fork with dynamic service registration](https://github.com/obsproject/obs-studio/pull/12911), required to use this plugin (see [Required OBS fork](#required-obs-fork) below).
* CMake 3.28+
* A C++ compiler with C++17 support (GCC 13+, Clang, or MSVC)

### Required OBS fork

To use this plugin, OBS Studio needs support for dynamically detecting services registered by plugins. That support is currently in review as [obs-studio PR #12911](https://github.com/obsproject/obs-studio/pull/12911), and is being considered for inclusion in the **v33.0** release (not guaranteed).

Until that PR is merged, you'll need to build OBS Studio from that PR's branch/fork to be able to select and configure this plugin's service from the OBS UI.

### Required libmoq fork

This plugin currently requires a fork of libmoq (openmoq/moq5): [qualabs/moq5](https://github.com/qualabs/moq5), whose codec-signaling helpers and sized config initializers are proposed upstream as [PR #5](https://github.com/openmoq/moq5/pull/5). The plugin will **not** build against upstream `moq5` as-is.

```bash
git clone https://github.com/qualabs/moq5.git
cd moq5
```

Then follow the build steps below from that checkout. The revision CI builds against is pinned in [`.github/scripts/.libmoq-version`](.github/scripts/.libmoq-version) and it is temporarily the `fix/hevc-temporal-sublayers` branch.

### Building libmoq (openmoq/moq5)

This plugin links against libmoq (openmoq/moq5) (`find_package(libmoq REQUIRED COMPONENTS service)`). libmoq must be built and installed on the system **before** configuring this plugin.

#### 1. Build and install libmoq

libmoq's picoquic-backed adapters (`MOQ_BUILD_PQ_THREADED`, used by the service tier here) need picoquic's private `picoquic_internal.h`, which picoquic does not install. Because of that, libmoq must be built in "source-tree mode": pointed at a picoquic source checkout plus a built picotls, rather than at any picoquic already installed on the system.

```bash
# Fetch pinned picoquic + picotls sources and build picotls.
# Prints (and writes to .deps/picoquic-ci/picoquic_deps.env) the two paths
# needed below.
scripts/setup_picoquic_deps.sh

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMOQ_BUILD_SERVICE=ON \
  -DMOQ_BUILD_MSF=ON \
  -DMOQ_BUILD_MEDIA_OBJECT=ON \
  -DMOQ_BUILD_ADAPTER_PICOQUIC=ON \
  -DMOQ_BUILD_PQ_THREADED=ON \
  -DMOQ_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DMOQ_PICOQUIC_SOURCE_DIR="$(pwd)/.deps/picoquic-ci/picoquic" \
  -DMOQ_PICOTLS_PREFIX="$(pwd)/.deps/picoquic-ci/picotls/build"

cmake --build build -j"$(nproc)"
cmake --install build
```

#### 2. Configure this plugin against libmoq

Because libmoq was built in source-tree mode, anything that consumes it via `find_package(libmoq)`, including this plugin, re-triggers the same picoquic resolution (`libmoqConfig.cmake` calls `find_dependency(Picoquic)`). So this plugin's own configure needs the same two picoquic variables, **and** needs OBS's default warnings-as-errors disabled.

```bash
cmake -S . -B build \
  -DMOQ_PICOQUIC_SOURCE_DIR=</path/to/moq5/>.deps/picoquic-ci/picoquic \
  -DMOQ_PICOTLS_PREFIX=</path/to/moq5/>.deps/picoquic-ci/picotls/build \
  -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF

cmake --build build -j"$(nproc)"
```

#### Building in CI

CI does the same thing automatically: [`.github/scripts/build-libmoq`](.github/scripts/build-libmoq) builds and installs libmoq into `.moq-deps/libmoq/prefix` at the revision pinned in `.github/scripts/.libmoq-version`, then exports `MOQ_PICOQUIC_SOURCE_DIR`, `MOQ_PICOTLS_PREFIX` and `CMAKE_PREFIX_PATH` for the plugin build that follows. To move to a different libmoq, change `MOQ5_REF` there to a commit SHA or a branch name; the build cache is keyed on the SHA it resolves to, so tracking a branch still picks up new commits.

On macOS the plugin is a universal binary, so libmoq and picotls are built for both `arm64` and `x86_64` in a single pass. OpenSSL cannot be built that way, its build system handles one architecture per tree, and neither obs-deps (mbedtls only) nor Homebrew (single-arch) ships a universal one, so [`.github/scripts/build-openssl-macos`](.github/scripts/build-openssl-macos) builds each slice separately and `lipo`s the static archives together. It runs before `build-libmoq` and exports `OPENSSL_ROOT_DIR`. OpenSSL is linked statically, so the plugin carries no Homebrew runtime dependency.

The Windows job is disabled (`if: false`) until libmoq is built there too.

#### Installing the built plugin

Copy the resulting shared object into your OBS install's plugin directory, e.g.:

```bash
cp build/obs-moq.so </path/to/obs-install>/lib/x86_64-linux-gnu/obs-plugins/
```

## Usage

1. Select the MOQ service in OBS's stream settings.
2. In the **Server** field, enter the URL of the MOQ relay you want to publish to.
3. In the **Stream Key** field, enter the MOQ namespace, with each namespace tuple part separated by a dash (`-`). For example, a namespace of `["live", "user123"]` would be entered as `live-user123`.

### Multitrack: publishing a second canvas

The MOQ service has an **Additional canvas (multitrack)** dropdown listing every
canvas OBS has registered.

With **None** (the default), nothing changes: one `video` track, one `audio`
track, on the stream key as the namespace. Pick a canvas and it is encoded in
parallel with the main one and published as **two video tracks in one
broadcast** — the common case being the portrait canvas contributed by the
[Aitum Vertical](https://github.com/Aitum/obs-vertical-canvas) plugin:

| Source canvas | Track | altGroup |
| --- | --- | --- |
| Main (landscape) | `video_main_1280x720` | 1 |
| Additional (portrait) | `video_extra_1080x1920` | 2 |
| — | `audio` (shared) | — |

One namespace, one MOQ session, one QUIC connection. A subscriber sees a single
broadcast and picks a track inside it, which is what MOQT's priorities assume:
§7 of draft-ietf-moq-transport-18 scopes them to ordering objects *within a
session*, so tracks that might need to be traded off against each other under
congestion belong on the same one.

**Why separate altGroups.** CMSF's altGroup marks tracks as quality alternatives
of one another. Landscape and portrait are different framings, not alternatives,
so they get different groups — otherwise a player's ABR treats them as a ladder
and drops from one orientation to the other to save bandwidth. Renditions of the
*same* canvas, if they are ever added, would share a group.

**Why a canvas UUID and not a plugin lookup.** libobs keeps a registry of every
canvas any plugin contributes, so there is nothing to link against and no reason
to know which plugin contributed what: the setting stores a canvas UUID and the
output resolves it with `obs_get_canvas_by_uuid()` at start time. This is what
OBS' own multitrack output does — Settings → Stream → *Additional canvas* stores
a UUID that `MultitrackVideoOutput` takes as `extra_canvas`. The dropdown lists
registered canvases that have a video mix, minus the main one (already published
from encoder slot 0) and minus ephemeral ones (previews, capture devices).

**How it is wired.** OBS outputs can take more than one video encoder
(`OBS_OUTPUT_MULTI_TRACK_VIDEO`), and every encoded packet arrives tagged with
the index of the encoder it came from. Slot 0 is the encoder the frontend
assigns, bound to the main canvas. When a canvas is selected, the plugin clones
that encoder (same id, same settings, so the service's encoder overrides carry
over) against the selected canvas' mix and puts it in slot 1, so each packet
routes to the track its encoder feeds.

Both encoders go into an `obs_encoder_group_t`. Ungrouped, each one treats the
first frame it happens to see as its own zero, and those zeros do not coincide —
encoder 0 is paired with the audio encoder and waits for it, while the second has
nothing to wait for. A group holds every member until they can all start on the
same timestamp. Independently of that, object timestamps are anchored on
`packet->dts_usec`, which libobs already puts on one clock, rather than on
`packet->pts`, which is a per-encoder frame counter.

Current limits of this POC:

* The second encoder clones the main encoder's settings, bitrate included. A
  per-canvas bitrate (Aitum has one) is not read.
* No per-track publisher priority: MOQT allows it (§7) and libmoq's publisher
  layer has it, but `moq_media_track_cfg_t` does not expose it yet — so
  "the vertical yields first under congestion" cannot be expressed.
* Only one additional canvas. The encoder slot is fixed at 1; libobs allows up
  to `MAX_OUTPUT_VIDEO_ENCODERS` (10), and OBS' own multitrack output has the
  same one-canvas limit today.
* The canvas size is read once, when the output starts. Resizing it mid-stream
  does not change what is published. Worse, libobs refuses the resize outright
  while *any* video output is active — `obs_canvas_reset_video()` returns false
  on `obs_video_active()` — and Aitum does not check that return value, so its UI
  and config can report a new size while its mix keeps the old one, and the
  plugin faithfully publishes the old one. Stop every output first (stream,
  recording, Aitum's backtrack, virtual camera), then resize, then start the
  stream.

### Relay issues found while testing

Both were reproduced against [moqx](https://github.com/openmoq/moqx) v0.3.4 with
`cache.enabled: true`, publishing from this plugin. Neither is caused by the
plugin, and neither has been filed yet.

**1. A stale catalog is served to subscribers that join after a republish.**

The catalog is a track like any other, so a relay caches its groups like any
other. When several publisher sessions use the same namespace over time, the
relay can hand a newly joined subscriber a catalog group from an earlier session.
That subscriber then sees the old track list — and only that list, because a
subscriber discovers tracks exclusively through the catalog: the tracks being
published right now exist on the relay, but nothing tells the subscriber they are
there.

* Relay config: `cache: {enabled: true, max_groups_per_track: 3}`.
* Publisher: this plugin, with `moq_media_sender_cfg_t.catalog_refresh_interval_us`
  set so the catalog is published once per session rather than periodically.
* Repro: publish namespace `ns` with a track named `video_main_1280x720`; stop; change
  the output resolution so the track is now named `video_main_1024x576`; publish `ns`
  again; subscribe a fresh player to `ns`.
* Expected: the catalog describes the tracks of the session currently publishing.
* Actual: the player lists `video_main_1280x720`, which no longer exists, and
  never learns about `video_main_1024x576`.
* Likely cause: the relay's cache is not invalidated when a publisher session for
  a namespace ends or when a new one announces it, and a subscriber is served a
  retained group instead of the most recent one.
* Workaround (what this plugin does now): republish the catalog every second,
  which is libmoq's default. The stale view then survives at most one second.
  MSF-01 §5 only requires a republish when the track set changes, so the periodic
  republish is pure repetition — it is papering over the relay behaviour.

**2. A Track Alias is recycled while still in use, killing the session.**

Switching video tracks repeatedly closes the subscriber's session. The relay
answers the new SUBSCRIBE with a Track Alias that is still live on the track
being replaced, and the subscriber closes the session with DUPLICATE_TRACK_ALIAS,
which draft-18 §11.1 requires of it.

* Repro: subscribe to one video track, then switch back and forth between two
  tracks of the same broadcast every few seconds. It survives a few switches and
  then dies — in our case on the fourth.
* Expected: the relay assigns an alias that is not mapped to another live track.
* Actual (player log, the three lines in order):

  ```
  [SWITCH] start: "video-1280x720" → "video-720x1280" (newReqId=6n, oldReqId=4n)
  [OBJ] video "video-1280x720" group=153n obj=1n alias=6n 567B
  [SESSION] closed: error=0x5 reason=Duplicate track alias 6
  ```

  Objects were still arriving on alias 6 for the old track at the moment the
  relay handed alias 6 to the new one.
* Why the race exists: players switch make-before-break — they subscribe to the
  new track, wait for a keyframe, splice, and only then unsubscribe from the old
  one — so both subscriptions are live for a moment and the relay recycles the
  alias inside that window.
* Impact: publishing is unaffected; only that viewer's session dies, and it has
  to reconnect. There is no workaround from the publisher side.

## Tested against

This plugin has been, and continues to be, tested using:

* [moqx](https://github.com/openmoq/moqx)
* [moq-playa](https://github.com/openmoq/moq-playa)

## Roadmap

The plugin currently only supports **H.264 video**. Planned next steps:

* Add support for audio tracks (AAC, Opus, AC-3)
* Add support for other video/audio codecs (HEVC, AV1)
* Add CMAF support
* Achieve a lower latency target

