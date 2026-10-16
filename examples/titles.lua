-- hyprtitles: window titles in a pill on the top border.
-- Copy to ~/.config/hypr/titles.lua and add require("hypr.titles") to hyprland.lua.

-- Text transformers. Each takes a string and returns a string.
local T = {}

function T.lower(s) return string.lower(s) end
function T.upper(s) return string.upper(s) end

-- "git log --oneline" -> "Git Log --oneline"
function T.ucwords(s)
  return (s:gsub("(%a)([%w_']*)", function(first, rest) return first:upper() .. rest end))
end

-- "git log" -> "Git log"
function T.ucfirst(s)
  return (s:gsub("^%l", string.upper))
end

function T.trim(s)
  return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- cut at n characters (UTF-8 aware) and add an ellipsis
function T.truncate(n)
  return function(s)
    if utf8.len(s) == nil or utf8.len(s) <= n then return s end
    return s:sub(1, utf8.offset(s, n + 1) - 1) .. "…"
  end
end

-- drop the "— Mozilla Firefox" style suffix browsers and editors append
-- (lua patterns work on bytes, so the multi-byte dashes are tried one by one)
function T.strip_app_suffix(s)
  for _, sep in ipairs({ " — ", " – ", " - ", " | " }) do
    local i, last = 0, nil
    while true do
      i = s:find(sep, i + 1, true)
      if not i then break end
      last = i
    end
    if last then return s:sub(1, last - 1) end
  end
  return s
end

-- "/home/dc/Projects/x" -> "~/Projects/x"
function T.tilde(s)
  local home = os.getenv("HOME")
  if not home then return s end
  return (s:gsub("^" .. home:gsub("%p", "%%%0"), "~"))
end

-- compose transformers left to right: T.chain(T.trim, T.lower)
function T.chain(...)
  local fns = { ... }
  return function(s)
    for _, f in ipairs(fns) do s = f(s) end
    return s
  end
end

-- What each window shows. Return:
--   a string           -> that text
--   nil                -> no pill on this window
--   { text, bg, fg }   -> text plus per-window colors ("rgb(f9e2af)", "rgba(...)", 0xffRRGGBB)
-- `w` is the same HL.Window object hl.get_windows() returns: w.title, w.class,
-- w.initial_title, w.active, w.floating, w.fullscreen, w.pinned, w.workspace, ...
local function label(w)
  if w.class == "" and w.title == "" then return nil end

  local title = T.chain(T.strip_app_suffix, T.tilde, T.trim)(w.title)

  if w.class == "chromium" or w.class == "firefox" then
    return T.truncate(40)(title)
  end

  return title
end

-- The plugin loads after the first config parse, so guard the plugin namespace.
if hl.plugin.hyprtitles ~= nil then
  hl.config({
    plugin = {
      hyprtitles = {
        font = "monospace",
        text_size = 11,
        text_weight = 500,
        padding_x = 8,
        padding_y = 1,
        max_width = 400,
        offset_x = 12,
        position = "top_right", -- top_/bottom_ right|left|center, left_/right_ top|bottom|center
        -- 0 = follow the window's border color / auto contrast text
        -- col = { active_bg = "rgb(f5a3c7)", inactive_bg = 0, active_text = 0, inactive_text = 0 },
        show_fullscreen = false,
        only_active = false,
        glow = true, -- extend the window's glow (decoration:shadow) around the pill
        inside = "auto", -- tab inside the window when there is no room above: auto, always, never
      },
    },
  })

  hl.plugin.hyprtitles.label(label)
end

return { T = T, label = label }
