#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FontWeightValue.hpp>

#include <string>
#include <vector>

struct lua_State;

inline HANDLE PHANDLE = nullptr;

class CTitleDeco;

struct SGlobalState {
    std::vector<WP<CTitleDeco>> decos;
    uint32_t                    noTitleRuleIdx = 0;

    // registry ref of the hl.plugin.hyprtitles.label() callback. Valid only for the
    // lua state it was created in; dropped on every config reload, because Hyprland
    // recreates the lua state and the config registers the callback again.
    struct {
        int        ref = -2; // LUA_NOREF
        lua_State* L   = nullptr;
    } label;

    // last callback error shown, so a broken callback doesn't spam notifications
    std::string lastLuaError;

    // label refresh runs in a wayland idle callback, never inside events or rendering
    bool                      refreshScheduled = false;
    UP<SEventLoopDoLaterLock> refreshLock;

    struct {
        SP<Config::Values::CBoolValue>       enabled, showFullscreen, onlyActive, glow;
        SP<Config::Values::CStringValue>     font, align;
        SP<Config::Values::CIntValue>        textSize, paddingX, paddingY, maxWidth, offsetX, offsetY;
        SP<Config::Values::CFontWeightValue> textWeight;
        SP<Config::Values::CColorValue>      activeBg, inactiveBg, activeText, inactiveText;
    } config;
};

inline UP<SGlobalState> g_pGlobalState;

void scheduleLabelRefresh();
