#define WLR_USE_UNSTABLE

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleEffectContainer.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/config/lua/bindings/LuaBindingsInternal.hpp>
#include <hyprland/src/config/lua/objects/LuaWindow.hpp>
#include <hyprland/src/config/lua/types/LuaConfigColor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <algorithm>

#include "TitleDeco.hpp"
#include "globals.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

static std::vector<CHyprSignalListener> g_listeners;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static CTitleDeco* decoFor(PHLWINDOW window) {
    for (auto& d : g_pGlobalState->decos) {
        if (d && d->getOwner() == window)
            return d.get();
    }
    return nullptr;
}

static bool hasLabelCallback() {
    return g_pGlobalState->label.ref != LUA_NOREF && g_pGlobalState->label.L;
}

static void markDirty(PHLWINDOW window) {
    if (auto* d = decoFor(window); d) {
        d->m_labelDirty = true;
        scheduleLabelRefresh();
    }
}

static void markAllDirty() {
    for (auto& d : g_pGlobalState->decos) {
        if (d)
            d->m_labelDirty = true;
    }
    scheduleLabelRefresh();
}

static void notifyLuaError(const std::string& err) {
    if (err == g_pGlobalState->lastLuaError)
        return;

    g_pGlobalState->lastLuaError = err;
    HyprlandAPI::addNotification(PHANDLE, "[hyprtitles] label callback failed, showing plain titles: " + err, CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
}

// Reads a color from the field `name` of the table at stack index `tableIdx`.
static std::optional<CHyprColor> tableColor(lua_State* L, int tableIdx, const char* name, std::string& err) {
    lua_getfield(L, tableIdx, name);
    Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });

    if (lua_isnil(L, -1))
        return std::nullopt;

    Config::Lua::CLuaConfigColor parser(0);
    if (parser.parse(L).errorCode != Config::Lua::PARSE_ERROR_OK) {
        err = std::format("field '{}' is not a valid color", name);
        return std::nullopt;
    }

    return CHyprColor{static_cast<uint64_t>(parser.parsed())};
}

// Runs the user's label callback for one window. Returns nullopt on any failure, so the
// caller falls back to the plain title.
static std::optional<SLabel> callLabelCallback(PHLWINDOW window) {
    lua_State* L   = g_pGlobalState->label.L;
    auto*      mgr = Config::Lua::CConfigManager::fromLuaState(L);
    if (!mgr)
        return std::nullopt;

    const int TOP = lua_gettop(L);
    Hyprutils::Utils::CScopeGuard restore([L, TOP] { lua_settop(L, TOP); });

    lua_rawgeti(L, LUA_REGISTRYINDEX, g_pGlobalState->label.ref);
    if (!lua_isfunction(L, -1))
        return std::nullopt;

    Config::Lua::Objects::CLuaWindow::push(L, PHLWINDOWREF{window});

    if (mgr->guardedPCall(1, 1, 0, Config::Lua::CConfigManager::LUA_TIMEOUT_EVENT_CALLBACK_MS, "hyprtitles label") != LUA_OK) {
        notifyLuaError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "unknown error");
        return std::nullopt;
    }

    SLabel label;

    if (lua_isnil(L, -1) || (lua_isboolean(L, -1) && !lua_toboolean(L, -1))) {
        label.hidden = true;
        return label;
    }

    if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER) {
        label.text = lua_tostring(L, -1);
        return label;
    }

    if (lua_istable(L, -1)) {
        const int   TABLE = lua_gettop(L);
        std::string err;

        lua_getfield(L, TABLE, "text");
        if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER)
            label.text = lua_tostring(L, -1);
        else if (lua_isnil(L, -1))
            label.text = window->m_title;
        else
            err = "field 'text' must be a string";
        lua_pop(L, 1);

        label.bg = tableColor(L, TABLE, "bg", err);
        label.fg = tableColor(L, TABLE, "fg", err);

        if (!err.empty()) {
            notifyLuaError(err);
            return std::nullopt;
        }

        return label;
    }

    notifyLuaError(std::format("expected a string, a table or nil, got {}", luaL_typename(L, -1)));
    return std::nullopt;
}

static SLabel resolveLabel(PHLWINDOW window) {
    if (hasLabelCallback()) {
        if (auto label = callLabelCallback(window); label)
            return *label;
    }

    return SLabel{.text = window->m_title};
}

static void refreshLabels() {
    g_pGlobalState->refreshScheduled = false;

    // copy: the callback can open or close windows, which changes the list
    const auto DECOS = g_pGlobalState->decos;

    for (const auto& d : DECOS) {
        if (!d || !d->m_labelDirty)
            continue;

        const auto PWINDOW = d->getOwner();
        if (!PWINDOW || !validMapped(PWINDOW))
            continue;

        auto label = resolveLabel(PWINDOW);

        if (d)
            d->setLabel(std::move(label));
    }
}

void scheduleLabelRefresh() {
    if (!g_pGlobalState || g_pGlobalState->refreshScheduled)
        return;

    g_pGlobalState->refreshScheduled = true;
    g_pGlobalState->refreshLock      = g_pEventLoopManager->doLaterLock([] {
        if (g_pGlobalState)
            refreshLabels();
    });
}

static void onNewWindow(PHLWINDOW window) {
    if (std::ranges::any_of(window->m_windowDecorations, [](const auto& d) { return d->getDisplayName() == "Hyprtitles"; }))
        return;

    auto deco = makeUnique<CTitleDeco>(window);
    g_pGlobalState->decos.emplace_back(deco);
    deco->m_self = deco;
    HyprlandAPI::addWindowDecoration(PHANDLE, window, std::move(deco));

    markDirty(window);
}

static void onUpdateWindowRules(PHLWINDOW window) {
    auto* d = decoFor(window);
    if (!d)
        return;

    d->updateRules();
    if (hasLabelCallback())
        markDirty(window);
}

static void onPreConfigReload() {
    // Hyprland destroys the lua state on reload, which also unrefs the callback
    // (we handed the ref to registerLuaRef). The new config registers it again.
    g_pGlobalState->label        = {};
    g_pGlobalState->lastLuaError = "";
}

static void onConfigReloaded() {
    for (auto& d : g_pGlobalState->decos) {
        if (d)
            d->onConfigReloaded();
    }
    markAllDirty();
}

// hl.plugin.hyprtitles.label(fn) sets the callback; label(nil) removes it.
static int luaLabel(lua_State* L) {
    auto* mgr = Config::Lua::CConfigManager::fromLuaState(L);
    if (!mgr)
        return luaL_error(L, "hyprtitles.label: config manager unavailable");

    if (!lua_isnoneornil(L, 1) && !lua_isfunction(L, 1))
        return Config::Lua::Bindings::Internal::configError(L, "hyprtitles.label: expected a function or nil");

    // A ref from an earlier call stays registered with Hyprland, which unrefs it on the
    // next reload; unref'ing it here too would free a registry slot twice.
    g_pGlobalState->label        = {};
    g_pGlobalState->lastLuaError = "";

    if (lua_isfunction(L, 1)) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
        lua_State* mainL = lua_tothread(L, -1);
        lua_pop(L, 1);

        lua_pushvalue(L, 1);
        const int REF = luaL_ref(L, LUA_REGISTRYINDEX);
        mgr->registerLuaRef(REF);

        g_pGlobalState->label = {.ref = REF, .L = mainL};
    }

    markAllDirty();
    return 0;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprtitles] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprtitles] Version mismatch");
    }

    g_pGlobalState                 = makeUnique<SGlobalState>();
    g_pGlobalState->noTitleRuleIdx = Desktop::Rule::windowEffects()->registerEffect("hyprtitles:no_title");

    auto& c          = g_pGlobalState->config;
    c.enabled        = makeShared<Config::Values::CBoolValue>("plugin:hyprtitles:enabled", "Whether title pills are drawn", true);
    c.font           = makeShared<Config::Values::CStringValue>("plugin:hyprtitles:font", "Font family of the title text", "monospace");
    c.textSize       = makeShared<Config::Values::CIntValue>("plugin:hyprtitles:text_size", "Title text size, in logical px", 11);
    c.textWeight     = makeShared<Config::Values::CFontWeightValue>("plugin:hyprtitles:text_weight", "Title text weight (e.g. \"bold\" or 100-1000)", 500);
    c.paddingX       = makeShared<Config::Values::CIntValue>("plugin:hyprtitles:padding_x", "Horizontal padding inside the pill", 8);
    c.paddingY       = makeShared<Config::Values::CIntValue>("plugin:hyprtitles:padding_y", "Vertical padding inside the pill", 1);
    c.maxWidth       = makeShared<Config::Values::CIntValue>("plugin:hyprtitles:max_width", "Longest text before it is cut with an ellipsis; 0 = window width", 400);
    c.offsetX        = makeShared<Config::Values::CIntValue>("plugin:hyprtitles:offset_x", "Distance from the rounded corner to the pill", 12);
    c.offsetY        = makeShared<Config::Values::CIntValue>("plugin:hyprtitles:offset_y", "Vertical shift from the border line; positive is down", 0);
    c.align          = makeShared<Config::Values::CStringValue>("plugin:hyprtitles:align", "Where on the top border when position is empty: right, left or center", "right");
    c.activeBg       = makeShared<Config::Values::CColorValue>("plugin:hyprtitles:col.active_bg", "Pill color of the focused window; 0 = its border color", 0);
    c.inactiveBg     = makeShared<Config::Values::CColorValue>("plugin:hyprtitles:col.inactive_bg", "Pill color of other windows; 0 = their border color", 0);
    c.activeText     = makeShared<Config::Values::CColorValue>("plugin:hyprtitles:col.active_text", "Text color of the focused window; 0 = auto contrast", 0);
    c.inactiveText   = makeShared<Config::Values::CColorValue>("plugin:hyprtitles:col.inactive_text", "Text color of other windows; 0 = auto contrast", 0);
    c.showFullscreen = makeShared<Config::Values::CBoolValue>("plugin:hyprtitles:show_fullscreen", "Draw the pill on fullscreen windows", false);
    c.onlyActive     = makeShared<Config::Values::CBoolValue>("plugin:hyprtitles:only_active", "Draw the pill only on the focused window", false);
    c.position       = makeShared<Config::Values::CStringValue>("plugin:hyprtitles:position",
                                                                  "Edge and corner: top_right, top_left, top_center, bottom_*, left_top, left_bottom, left_center, right_*; "
                                                                  "empty = top, aligned by align",
                                                                  "");
    c.inside         = makeShared<Config::Values::CStringValue>("plugin:hyprtitles:inside",
                                                                "Hang the label inside the window as a tab: auto (when there is no room above), always or never", "auto");
    c.glow           = makeShared<Config::Values::CBoolValue>("plugin:hyprtitles:glow", "Extend the window's glow (decoration:shadow) around the pill", true);

    for (const auto& v : std::initializer_list<SP<Config::Values::IValue>>{c.enabled, c.font, c.textSize, c.textWeight, c.paddingX, c.paddingY, c.maxWidth, c.offsetX, c.offsetY,
                                                                           c.align, c.activeBg, c.inactiveBg, c.activeText, c.inactiveText, c.showFullscreen, c.onlyActive, c.glow, c.inside, c.position}) {
        HyprlandAPI::addConfigValueV2(PHANDLE, v);
    }

    if (Config::mgr()->type() == Config::CONFIG_LUA)
        HyprlandAPI::addLuaFunction(PHANDLE, "hyprtitles", "label", ::luaLabel);

    auto& bus = Event::bus()->m_events;
    g_listeners.push_back(bus.window.open.listen([](PHLWINDOW w) { onNewWindow(w); }));
    g_listeners.push_back(bus.window.title.listen([](PHLWINDOW w) { markDirty(w); }));
    g_listeners.push_back(bus.window.class_.listen([](PHLWINDOW w) { markDirty(w); }));
    g_listeners.push_back(bus.window.updateRules.listen([](PHLWINDOW w) { onUpdateWindowRules(w); }));
    g_listeners.push_back(bus.window.active.listen([](PHLWINDOW, Desktop::eFocusReason) {
        if (hasLabelCallback())
            markAllDirty();
    }));
    for (auto* ev : {&bus.window.floating, &bus.window.fullscreen, &bus.window.pin}) {
        g_listeners.push_back(ev->listen([](PHLWINDOW w) {
            if (hasLabelCallback())
                markDirty(w);
        }));
    }
    g_listeners.push_back(bus.window.moveToWorkspace.listen([](PHLWINDOW w, PHLWORKSPACE) {
        if (hasLabelCallback())
            markDirty(w);
    }));
    g_listeners.push_back(bus.config.preReload.listen([] { onPreConfigReload(); }));
    g_listeners.push_back(bus.config.reloaded.listen([] { onConfigReloaded(); }));

    for (auto& w : Desktop::windowState()->windows()) {
        if (w->isHidden() || !validMapped(w))
            continue;

        onNewWindow(w);
    }

    // re-run the config so plugin:hyprtitles values and the label callback apply
    HyprlandAPI::reloadConfig();

    return {"hyprtitles", "Window titles in a pill on the window border.", "Ciprian Dobrea", "0.2.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listeners.clear();
    g_pGlobalState->refreshLock.reset();
    g_pGlobalState->refreshScheduled = false;
    g_pGlobalState->label            = {};

    g_pHyprRenderer->m_renderPass.removeAllOfType("CTitlePassElement");

    Desktop::Rule::windowEffects()->unregisterEffect(g_pGlobalState->noTitleRuleIdx);

    // g_pGlobalState stays alive: Hyprland removes our decorations after this returns,
    // and they still read the config while being damaged away.
}
