# Scrolling capture — design spec and field learnings

Everything below was learned the hard way on real pages (ssp.sh, watson.ch,
LinkedIn, Obsidian, foot/less, a synthetic hostile page). If this feature is
ever reimplemented, start here.

## The problem

Capture the entire scrollable content of one window on Wayland/Hyprland as a
single tall image, from a compositor-level screenshot tool with no browser
extension, no CDP, and no DOM access. Works by paging the window and
stitching the frames by image content.

## Non-negotiable environmental facts

1. **No hardcoded geometry.** Windows are tiled at arbitrary sizes (full,
   half, quarter, eighth), monitors have fractional scales (e.g. 1.6), and
   every derived value (overlap, bands, park corner) must be computed from
   the actual window and monitor each run.
2. **Key injection**: `hyprctl dispatch "hl.dsp.send_shortcut({ mods = '',
   key = 'Page_Down', window = 'address:0x…' })"` delivers a key directly to
   a window by address without changing focus (Hyprland ≥ 0.56 Lua config;
   the classic `sendshortcut` string syntax no longer parses).
3. **Frames**: repeated one-shot `ext-image-copy-capture` output captures,
   cropped to the window's native-pixel rect
   (`(logical rect − monitor origin) × scale`, intersected with the output).
4. **Page_Down step size is app-controlled and can be brutal**: Chromium in
   a quarter-tile leaves only ~12 native rows of overlap between pages. Any
   fixed minimum-overlap constant will eventually put the true offset
   outside the search range. The floor must scale with the viewport
   (`content/80`, min 8).

## The core model (v2): stitch the region that moves, not the window

v1 stitched full-window strips and assumed uniform scroll. Multi-pane apps
(Obsidian: editor + sidebars; IDEs; file managers) break this — static side
panes get re-appended with every strip. The correct model:

1. Capture frame 0, send one Page_Down, capture frame 1.
2. Build a static-cell mask (same sample at same position in both frames).
3. The **scroll region** is the largest contiguous run of moving columns,
   and within it the largest contiguous run of moving rows. Snap edges to
   the window border when motion reaches the outermost samples. Fall back
   to the whole window if the region is implausibly small (<25% of area).
4. Everything afterwards — matching, stitching, bottom detection — operates
   on the region crop. Output = region-width top band from frame 0 (pane
   chrome), the stitched region, region-width bottom band from the last
   frame. Side panes and window chrome are simply not part of the output —
   which is also what users actually want (browser chrome excluded).

## Frame alignment (the matcher)

Signature: 64 luma samples per row across the interior 4–92% of the region
width (skips the auto-hiding scrollbar band). All costs are per-sample mean
absolute luma differences.

Layered defenses, each one earned by a real failure:

1. **Winsorized columns** (cap each column's contribution at 40): a small
   fixed widget riding over content — watson's floating avatar — cannot
   drown a row.
2. **Static-cell mask**: cells identical at the same position in both
   frames (fixed share bars, sticky chrome, empty margins) are excluded
   from every offset's cost. A fixed overlay can neither veto the true
   offset (watson's share bar sat exactly inside the one-line overlap a
   full page leaves) nor support a false one.
3. **Texture gate**: a row votes only if either side has content variation
   (max−min > 12) among unmasked cells. Blank-on-blank pairs cost zero at
   every offset (interline gaps, empty margins) and enough of them elect an
   arbitrary alignment — observed: cost 0.00 at offset 20 on a page that
   scrolled 799.
4. **Trimmed mean over rows** (drop worst 30%): animated ad banners and
   video players repaint one band every frame and would veto exact
   alignments under a plain mean.
5. **Fallback chain**: gated+masked cost (≥8 informative rows) → masked
   without texture gate (low-texture content: paging through a large
   photo) → plain cost. Never fall back past the mask while a static side
   pane could be in view (v2's region crop makes this near-moot).
6. **Full offset search, step 1**: sharp content only dips at the exact
   offset; a strided search steps right over it (no spatial
   autocorrelation in text).
7. **Near-best tie-breaking**: among candidates within 0.5 of the minimum
   cost, the smallest offset wins — it rests on the largest overlap.
8. **Acceptance tolerance 6.0** (mean luma): a clean match is <1, a
   misalignment >15 after the filtering above; fractional monitor scales
   put scroll offsets between sample rows and ad-heavy pages keep a floor
   of churn even at the true offset.
9. **"Nothing moved" (bottom) is judged by the same gated cost** as every
   offset, otherwise cheap blank rows fake a bottom mid-page.

## Sticky in-page bands

Headers/footers pinned inside the scrolling region (site navs, cookie
banners) are detected as maximal static runs from the region's top/bottom
(row-wise, tolerance 3.0, cap 30%) and excluded from the offset search.
Appended strips are the freshly revealed rows just above the footer, so a
sticky header appears exactly once at the top and a sticky footer exactly
once at the bottom (footer band is re-taken from the final frame).

## Capture cadence

- **Settle loop instead of fixed delays**: after each Page_Down, recapture
  every 70 ms until two consecutive frames "mostly agree" (trimmed row cost
  < 1.0 — byte-equality never settles on pages with animated ads), timeout
  900 ms. The *first* frame settles too: the capture overlay's fade-out
  repaints the screen for a few hundred ms after dismissal (no `no_anim`
  layer rule can be assumed).
- **Park the pointer** in a monitor corner outside the window during
  capture and restore it afterwards. Hover styling follows the pointer
  through the scrolling page (link highlights, the browser's link-target
  status bubble) and repaints a fresh artifact into every frame. Prefer
  bottom corners (the bar lives at the top).
- **Restore the scroll position** after the capture with one Page_Up per
  Page_Down sent (best-effort, no settling). Without this, a second capture
  finds the page already at the bottom and correctly-but-uselessly returns
  a single frame.
- One retry per step after 250 ms extra settling; two consecutive failed
  matches end the capture with what was stitched so far.
- Caps: 80 pages, 32000 px stitched height. Qt's image allocation limit
  must be lifted (`QImageReader::setAllocationLimit(0)`) or tall results
  cannot be reopened.

## Entry points

- Overlay: `S` arms scroll picking (same interaction as window mode, own
  badge); click or Enter on a window fires. The overlay must fully close
  before frames are captured (it would photograph its own veil), so the
  request is returned to `main()` and executed after the event loop ends;
  the stitched result opens in a second editor session in the same process.
- CLI: `omasnap scroll` / `--capture-scroll` captures the focused window
  directly (for a Hyprland binding); `--copy`/`--save` bypass the editor.
- Instance lock: scroll capture takes over a running overlay (EditFile
  semantics) and waits ~300 ms for it to unmap if it signalled one.

## Diagnostics

`OMASNAP_SCROLL_DEBUG=<dir>` creates the directory, dumps every captured
frame as `scroll-frame-NN.png`, and logs per-frame match data (offset,
header/footer rows, still/best costs). This turned every "it doesn't work"
report into a two-minute diagnosis; keep it.

## Known limitations (accepted)

- Apps that only scroll on mouse wheel (some chat clients) and terminal
  *scrollback* (Shift+PageUp) are not paged; apps where Page_Down moves a
  caret instead of the view (editors in edit mode) scroll by caret.
- Pages with parallax/scroll-linked transforms cannot be aligned globally;
  the trimmed cost tolerates small bands, nothing more.
- Infinite feeds end at the page/height caps, not at a "bottom".
- The last partial page may duplicate a few rows on zero-texture content
  (accepted: duplication is invisible on blank regions).

## Test matrix that must pass before shipping

1. Static site, full-height and quarter-tile browser windows (ssp.sh).
2. Ad-heavy news article with fixed overlays (watson.ch article).
3. Synthetic hostile page: spinner ads, marquee, fixed fake video overlay,
   lazy images, deterministic per-row content (in-repo test page).
4. Multi-pane app: Obsidian note with sidebars — sidebar must appear
   exactly once, seams unbroken across headings.
5. Terminal pager: `foot -e less -z-10 <wide unique-line file>`.
6. Short/unscrollable page → single clean frame, instant.
7. Page already at bottom → after restore, a re-run still captures the
   full page.
8. Headless smoke: synthetic stitcher reconstruction (exact), S-arming
   flow, false-bottom rejection, height cap.
