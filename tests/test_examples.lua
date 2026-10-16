-- Runs examples/titles.lua outside Hyprland with a stub `hl` and checks the transformers
-- and the label function. Usage: lua tests/test_examples.lua (from the repo root)

local registered
hl = {
  config = function() end,
  plugin = { hyprtitles = { label = function(fn) registered = fn end } },
}

local mod = dofile("examples/titles.lua")
local T, label = mod.T, mod.label

local failures = 0
local function eq(name, got, want)
  if got ~= want then
    failures = failures + 1
    print(string.format("FAIL %s: got %q, want %q", name, tostring(got), tostring(want)))
  end
end

eq("lower", T.lower("Git LOG"), "git log")
eq("upper", T.upper("git log"), "GIT LOG")
eq("ucwords", T.ucwords("git log --oneline"), "Git Log --Oneline")
eq("ucwords keeps apostrophe words", T.ucwords("don't stop"), "Don't Stop")
eq("ucfirst", T.ucfirst("git log"), "Git log")
eq("ucfirst non-letter", T.ucfirst("1 git"), "1 git")
eq("trim", T.trim("  a b  "), "a b")
eq("truncate short", T.truncate(10)("abc"), "abc")
eq("truncate long", T.truncate(3)("abcdef"), "abc…")
eq("truncate utf8", T.truncate(2)("ăîșț"), "ăî…")
eq("strip suffix em dash", T.strip_app_suffix("Inbox — Mozilla Firefox"), "Inbox")
eq("strip suffix dash", T.strip_app_suffix("main.c - Visual Studio Code"), "main.c")
eq("strip keeps plain", T.strip_app_suffix("git log"), "git log")
eq("tilde", T.tilde(os.getenv("HOME") .. "/Projects/x"), "~/Projects/x")
eq("chain", T.chain(T.trim, T.upper)("  ab "), "AB")

eq("label registered", registered, label)
eq("label plain", label({ class = "foot", title = "git log" }), "git log")
eq("label empty window", label({ class = "", title = "" }), nil)
eq("label browser", label({ class = "firefox", title = string.rep("x", 50) .. " — Mozilla Firefox" }), string.rep("x", 40) .. "…")

if failures > 0 then
  print(failures .. " failure(s)")
  os.exit(1)
end
print("all tests passed")
