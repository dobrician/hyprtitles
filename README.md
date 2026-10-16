# hyprtitles

A Hyprland plugin that draws each window's title in a small pill sitting on the
top border, in the right corner by default. The pill takes the window's border
color, so the focused window's label changes color along with its border, and it
follows the theme.

The text comes from a Lua function in your config, so you decide what each
window shows: lowercase, uppercase, title case, cut to a length, a different
label per app, or no pill at all.

Built for Hyprland 0.56 with the Lua config (Omarchy). The rendering is adapted
from [hyprbars](https://github.com/hyprwm/hyprland-plugins/tree/main/hyprbars).

## Install

With hyprpm:

```sh
hyprpm update                  # builds headers for the running Hyprland, once
hyprpm add https://github.com/dobrician/hyprtitles   # or a local path to this repo
hyprpm enable hyprtitles
hyprpm reload -n
```

To load it at login, add to `~/.config/hypr/autostart.lua`:

```lua
hl.exec_cmd("hyprpm reload -n")
```

After a Hyprland update the plugin has to be rebuilt against the new version,
which `hyprpm update` does. On Omarchy, a post-update hook can run it for you:

```sh
cp extras/omarchy-post-update-hyprpm ~/.config/omarchy/hooks/post-update.d/hyprpm-update
```

Without hyprpm: `make`, then `hl.plugin.load("/path/to/hyprtitles.so")` in your config.

## Configure

Copy `examples/titles.lua` to `~/.config/hypr/titles.lua` and add
`require("hypr.titles")` to `~/.config/hypr/hyprland.lua`.

The plugin loads after the first config parse, so wrap plugin settings in
`if hl.plugin.hyprtitles ~= nil then ... end`. Loading the plugin reloads the
config, and the block runs then.

### Options

```lua
hl.config({ plugin = { hyprtitles = {
  enabled = true,
  font = "monospace",
  text_size = 11,          -- logical px
  text_weight = 500,       -- or "bold"
  padding_x = 8,
  padding_y = 1,
  max_width = 400,         -- longer text is cut with "…"; 0 = window width
  position = "top_right",  -- see below
  offset_x = 12,           -- distance from the rounded corner
  offset_y = 0,            -- shift from the border line, positive is into the window
  col = {
    active_bg = 0,         -- 0 = the window's border color
    inactive_bg = 0,
    active_text = 0,       -- 0 = black or white, whichever reads better
    inactive_text = 0,
  },
  show_fullscreen = false,
  only_active = false,
  glow = true,             -- extend the window's glow around the pill
  inside = "auto",         -- tab inside the window: auto, always, never
} } })
```

### Position

`position` is the edge, then where along it:

- `top_right`, `top_left`, `top_center`
- `bottom_right`, `bottom_left`, `bottom_center`
- `left_top`, `left_bottom`, `left_center`
- `right_top`, `right_bottom`, `right_center`

On the left and right edges the text runs along the edge, bottom to top on the
left and top to bottom on the right. Spaces or dashes work too (`"left top"`).
Left empty, the label goes on the top edge, placed by the older
`align = "right" | "left" | "center"`.

### No room past the edge

When there is no room past the label's edge, because the window touches the
edge of the screen or a bar, or a tiled window sits right next to it with no
gaps, the label moves inside the window: a tab hanging from the border, rounded
at its free end and joined to the border with inner curves. `inside = "always"` always draws
it that way, `inside = "never"` never does. If the window glows, the glow
falls from the tab into the window.

### Glow

The pill glows only when its window glows (`decoration:shadow`), with the
same color, range, power, offset and fade. A transparent `color_inactive`
means no glow on inactive windows, so none on their pills either. Like the
window's own glow, it stays outside the window. `glow = false` turns it off; a `sharp`
shadow is not copied.

### What the pill says

```lua
hl.plugin.hyprtitles.label(function(w)
  if w.class == "steam" then return nil end           -- no pill
  if w.class == "foot" then
    return { text = string.upper(w.title), bg = "rgb(f9e2af)" }
  end
  return string.lower(w.title)
end)
```

The function gets the same window object as `hl.get_windows()` (`w.title`,
`w.class`, `w.initial_title`, `w.active`, `w.floating`, `w.fullscreen`,
`w.workspace`, ...). It returns:

- a string: the text to show
- `nil` or `false`: no pill on that window
- a table `{ text = ..., bg = ..., fg = ... }`: text plus colors for that window;
  `text` defaults to the title

`examples/titles.lua` has ready-made transformers: `lower`, `upper`, `ucwords`,
`ucfirst`, `trim`, `truncate(n)`, `strip_app_suffix`, `tilde`, and `chain(...)`
to combine them.

The function runs when a window opens, changes title or class, gains or loses
focus, floats, goes fullscreen, moves workspace, and after a config reload. It
never runs while a frame is drawn. It has 50 ms per call; an error, a timeout
or a bad return value shows a notification and that window falls back to its
plain title.

### Window rule

`hyprtitles:no_title` hides the pill on matching windows.

## Development

```sh
make          # builds hyprtitles.so against the installed Hyprland headers
make test     # tests the transformers in examples/titles.lua (needs lua)
```

To try a build without risking your session, run a nested Hyprland in a window
with a test config that calls `hl.plugin.load()` on the fresh `.so`.
