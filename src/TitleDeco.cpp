#include "TitleDeco.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRule.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <cmath>

#include "TitlePassElement.hpp"

using namespace Render::GL;

static CHyprColor configColor(Config::INTEGER color) {
    return CHyprColor{static_cast<uint64_t>(color)};
}

static CHyprColor lerpColor(const CHyprColor& from, const CHyprColor& to, float t) {
    return CHyprColor{from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t, from.b + (to.b - from.b) * t, from.a + (to.a - from.a) * t};
}

static CHyprColor firstColor(const Config::CGradientValueData& grad) {
    return grad.m_colors.empty() ? CHyprColor{0.5F, 0.5F, 0.5F, 1.F} : grad.m_colors.front();
}

static bool hasAlpha(const Config::CGradientValueData& grad) {
    return std::ranges::any_of(grad.m_colors, [](const auto& c) { return c.a > 0.001F; });
}

// The window's glow is Hyprland's decoration:shadow. These are the settings the pill glow
// copies; range is 0 when there is no glow to extend.
struct SGlowConfig {
    int      range = 0;
    float    scale = 1.F;
    Vector2D offset;
};

static SGlowConfig glowConfig() {
    if (!g_pGlobalState->config.glow->value())
        return {};

    const CConfigValue<Config::INTEGER> ENABLED("decoration:shadow:enabled");
    const CConfigValue<Config::INTEGER> SHARP("decoration:shadow:sharp");
    const CConfigValue<Config::INTEGER> RANGE("decoration:shadow:range");
    const CConfigValue<Config::FLOAT>   SCALE("decoration:shadow:scale");
    const CConfigValue<Config::VEC2>    OFFSET("decoration:shadow:offset");

    // a sharp shadow is a flat offset copy of the window, not a glow
    if (*ENABLED != 1 || *SHARP || *RANGE <= 0)
        return {};

    return {.range = static_cast<int>(*RANGE), .scale = std::clamp(*SCALE, 0.F, 1.F), .offset = Vector2D{(*OFFSET).x, (*OFFSET).y}};
}

// how far the glow reaches past the pill, for damage and extents (logical px)
double glowReach() {
    const auto GLOW = glowConfig();
    if (GLOW.range <= 0)
        return 0;
    return GLOW.range + std::max(std::abs(GLOW.offset.x), std::abs(GLOW.offset.y));
}

CTitleDeco::CTitleDeco(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow), m_window(pWindow) {
    m_label.text = pWindow->m_title;
}

CTitleDeco::~CTitleDeco() {
    if (g_pGlobalState)
        std::erase(g_pGlobalState->decos, m_self);
}

SDecorationPositioningInfo CTitleDeco::getPositioningInfo() {
    // Absolute and non-solid: the pill overlaps the border and reserves no space, so the
    // layout and the other decorations stay where they are. The extents only widen the
    // window's damage box so the part of the pill above the border gets repainted.
    SDecorationPositioningInfo info;
    info.policy         = DECORATION_POSITION_ABSOLUTE;
    info.edges          = DECORATION_EDGE_TOP;
    info.reserved       = false;
    info.desiredExtents = {Vector2D{0.0, overhang()}, Vector2D{0.0, 0.0}};
    return info;
}

void CTitleDeco::onPositioningReply(const SDecorationPositioningReply& reply) {
    ;
}

std::string CTitleDeco::getDisplayName() {
    return "Hyprtitles";
}

eDecorationType CTitleDeco::getDecorationType() {
    return DECORATION_CUSTOM;
}

eDecorationLayer CTitleDeco::getDecorationLayer() {
    // the border is also on LAYER_OVER and is added before us, so the pill draws on top of it
    return DECORATION_LAYER_OVER;
}

uint64_t CTitleDeco::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

PHLWINDOW CTitleDeco::getOwner() {
    return m_window.lock();
}

void CTitleDeco::updateWindow(PHLWINDOW) {
    damageEntire();
}

void CTitleDeco::damageEntire() {
    const auto PWINDOW = m_window.lock();
    if (!PWINDOW || !validMapped(PWINDOW))
        return;

    const auto BOX = pillBoxGlobal(PWINDOW);
    if (BOX.w > 0 && BOX.h > 0)
        g_pHyprRenderer->damageBox(BOX.copy().expand(2 + glowReach()));
}

void CTitleDeco::setLabel(SLabel&& label) {
    m_labelDirty = false;

    if (label == m_label)
        return;

    damageEntire();
    m_label = std::move(label);
    damageEntire();
}

void CTitleDeco::updateRules() {
    const auto PWINDOW = m_window.lock();
    if (!PWINDOW)
        return;

    const bool PREV = m_noTitleRule;
    m_noTitleRule   = false;

    const auto& PROPS = PWINDOW->m_ruleApplicator->m_otherProps.props;
    if (PROPS.contains(g_pGlobalState->noTitleRuleIdx))
        m_noTitleRule = truthy(PROPS.at(g_pGlobalState->noTitleRuleIdx)->effect);

    if (PREV != m_noTitleRule)
        damageEntire();
}

void CTitleDeco::onConfigReloaded() {
    m_textTex      = nullptr;
    m_texKey       = {};
    m_labelDirty   = true;
    m_lastOverhang = -1;
    g_pDecorationPositioner->repositionDeco(this);
    damageEntire();
}

bool CTitleDeco::isActive(PHLWINDOW w) {
    return Desktop::focusState()->window() == w;
}

bool CTitleDeco::shouldShow(PHLWINDOW w) {
    if (!g_pGlobalState->config.enabled->value() || m_noTitleRule || m_label.hidden || m_label.text.empty())
        return false;

    if (!w->m_ruleApplicator->decorate().valueOrDefault() || w->m_X11DoesntWantBorders)
        return false;

    if (!g_pGlobalState->config.showFullscreen->value() && Fullscreen::controller()->getFullscreenModes(w).internal == Fullscreen::FSMODE_FULLSCREEN)
        return false;

    if (g_pGlobalState->config.onlyActive->value() && !isActive(w))
        return false;

    return true;
}

CHyprColor CTitleDeco::backgroundColor(PHLWINDOW w) {
    if (m_label.bg)
        return *m_label.bg;

    const auto CONFIGURED = isActive(w) ? g_pGlobalState->config.activeBg->value() : g_pGlobalState->config.inactiveBg->value();
    if (CONFIGURED != 0)
        return configColor(CONFIGURED);

    // follow the border, including its active <-> inactive fade
    const auto CURRENT = firstColor(w->m_realBorderColor);
    if (w->m_borderFadeAnimationProgress && w->m_borderFadeAnimationProgress->isBeingAnimated())
        return lerpColor(firstColor(w->m_realBorderColorPrevious), CURRENT, w->m_borderFadeAnimationProgress->value());

    return CURRENT;
}

CHyprColor CTitleDeco::textColor(PHLWINDOW w) {
    if (m_label.fg)
        return *m_label.fg;

    const auto CONFIGURED = isActive(w) ? g_pGlobalState->config.activeText->value() : g_pGlobalState->config.inactiveText->value();
    if (CONFIGURED != 0)
        return configColor(CONFIGURED);

    // auto contrast against the settled background, not the animated one, so the text
    // texture isn't re-rendered on every frame of a border fade
    CHyprColor bg;
    if (m_label.bg)
        bg = *m_label.bg;
    else {
        const auto BGCONF = isActive(w) ? g_pGlobalState->config.activeBg->value() : g_pGlobalState->config.inactiveBg->value();
        bg                = BGCONF != 0 ? configColor(BGCONF) : firstColor(w->m_realBorderColor);
    }

    const double LUMA = 0.2126 * bg.r + 0.7152 * bg.g + 0.0722 * bg.b;
    return LUMA > 0.5 ? CHyprColor{0.1F, 0.1F, 0.1F, 1.F} : CHyprColor{0.95F, 0.95F, 0.95F, 1.F};
}

int CTitleDeco::maxTextWidth(PHLWINDOW w) {
    // logical px available for the text: the window's top edge minus the rounded corners,
    // the offset from the corner and the pill padding, capped by max_width
    const auto   WINBOX   = w->getWindowMainSurfaceBox();
    const double ROUNDING = w->rounding();
    const double OFFSET   = std::max<Config::INTEGER>(0, g_pGlobalState->config.offsetX->value());
    const double PADDING  = std::max<Config::INTEGER>(0, g_pGlobalState->config.paddingX->value());
    double       avail    = WINBOX.w - 2 * ROUNDING - 2 * OFFSET - 2 * PADDING;

    const auto   MAXW = g_pGlobalState->config.maxWidth->value();
    if (MAXW > 0)
        avail = std::min<double>(avail, MAXW);

    return std::max(0, static_cast<int>(std::floor(avail)));
}

void CTitleDeco::ensureTexture(PHLWINDOW w, float scale) {
    const auto  MAXW   = maxTextWidth(w);
    const auto  SIZE   = static_cast<int>(g_pGlobalState->config.textSize->value());
    const auto  WEIGHT = static_cast<int>(g_pGlobalState->config.textWeight->value().m_value);
    const auto  FONT   = g_pGlobalState->config.font->value();
    const auto  COLOR  = textColor(w);
    const int   MAXPX  = static_cast<int>(std::floor(MAXW * scale));

    const bool  SAMEBASE = m_textTex && m_textTex->m_texID != 0 && m_texKey.text == m_label.text && m_texKey.font == FONT && m_texKey.color == COLOR && m_texKey.scale == scale &&
        m_texKey.size == SIZE && m_texKey.weight == WEIGHT;

    // a new width limit only matters if the text was cut, or no longer fits
    const bool  WIDTHOK = m_texKey.maxWidthPx == MAXPX || (!m_textTruncated && m_textTex && m_textTex->m_size.x <= MAXPX);

    if (SAMEBASE && WIDTHOK)
        return;

    m_texKey = {.text = m_label.text, .font = FONT, .color = COLOR, .scale = scale, .size = SIZE, .weight = WEIGHT, .maxWidthPx = MAXPX};

    if (MAXPX < 1 || SIZE < 1) {
        m_textTex       = nullptr;
        m_textTruncated = false;
        return;
    }

    m_textTex = g_pHyprRenderer->renderText(m_label.text, COLOR, std::round(SIZE * scale), false, FONT, MAXPX, WEIGHT);

    // pango ellipsizes at maxWidth; a texture as wide as the limit was most likely cut
    m_textTruncated = m_textTex && m_textTex->m_size.x >= MAXPX - 1;

    const double NEWOVERHANG = overhang();
    if (std::abs(NEWOVERHANG - m_lastOverhang) > 0.5) {
        m_lastOverhang = NEWOVERHANG;
        g_pDecorationPositioner->repositionDeco(this);
    }
}

double CTitleDeco::overhang() {
    // how far the pill reaches above the window's main surface (logical px)
    const auto PWINDOW = m_window.lock();
    if (!PWINDOW)
        return 0;

    const double TEXTH  = m_textTex && m_texKey.scale > 0 ? m_textTex->m_size.y / m_texKey.scale : g_pGlobalState->config.textSize->value() * 1.4;
    const double PILLH  = TEXTH + 2 * std::max<Config::INTEGER>(0, g_pGlobalState->config.paddingY->value());
    const double BORDER = PWINDOW->getRealBorderSize();

    return std::max(0.0, BORDER / 2.0 + PILLH / 2.0 - g_pGlobalState->config.offsetY->value()) + 2 + glowReach();
}

CBox CTitleDeco::pillBoxGlobal(PHLWINDOW w) {
    if (!m_textTex || m_texKey.scale <= 0)
        return {};

    const auto   WINBOX   = w->getWindowMainSurfaceBox();
    const double SCALE    = m_texKey.scale;
    const double PADX     = std::max<Config::INTEGER>(0, g_pGlobalState->config.paddingX->value());
    const double PADY     = std::max<Config::INTEGER>(0, g_pGlobalState->config.paddingY->value());
    const double OFFSET   = std::max<Config::INTEGER>(0, g_pGlobalState->config.offsetX->value());
    const double ROUNDING = w->rounding();
    const double BORDER   = w->getRealBorderSize();

    const double PILLW = m_textTex->m_size.x / SCALE + 2 * PADX;
    const double PILLH = m_textTex->m_size.y / SCALE + 2 * PADY;

    // vertically centered on the top border line
    const double Y = WINBOX.y - BORDER / 2.0 - PILLH / 2.0 + g_pGlobalState->config.offsetY->value();

    double       x     = 0;
    const auto   ALIGN = g_pGlobalState->config.align->value();
    if (ALIGN == "left")
        x = WINBOX.x + ROUNDING + OFFSET;
    else if (ALIGN == "center")
        x = WINBOX.x + (WINBOX.w - PILLW) / 2.0;
    else
        x = WINBOX.x + WINBOX.w - ROUNDING - OFFSET - PILLW;

    CBox       box = {x, Y, PILLW, PILLH};

    const auto PWORKSPACE = w->m_workspace;
    if (PWORKSPACE && !w->m_pinned)
        box.translate(PWORKSPACE->m_renderOffset->value());

    return box;
}

void CTitleDeco::draw(PHLMONITOR pMonitor, const float& a) {
    const auto PWINDOW = m_window.lock();
    if (!PWINDOW || !validMapped(PWINDOW) || !shouldShow(PWINDOW))
        return;

    auto data = CTitlePassElement::STitleData{this, a};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTitlePassElement>(data));
}

void CTitleDeco::renderPass(PHLMONITOR pMonitor, const float& a) {
    const auto PWINDOW = m_window.lock();
    if (!PWINDOW || !pMonitor || !validMapped(PWINDOW) || !shouldShow(PWINDOW))
        return;

    const float SCALE = pMonitor->m_scale;
    ensureTexture(PWINDOW, SCALE);

    if (!m_textTex || m_textTex->m_texID == 0)
        return;

    CBox box = pillBoxGlobal(PWINDOW);
    box.translate(PWINDOW->m_floatingOffset - pMonitor->m_position).scale(SCALE).round();

    if (box.w < 1 || box.h < 1)
        return;

    renderGlow(PWINDOW, pMonitor, box, a);

    CHyprColor bg = backgroundColor(PWINDOW);
    bg.a *= a;

    g_pHyprOpenGL->renderRect(box, bg, {.round = static_cast<int>(std::floor(box.h / 2.0)), .roundingPower = 2.F});

    CBox textBox = {box.x + (box.w - m_textTex->m_size.x) / 2.0, box.y + (box.h - m_textTex->m_size.y) / 2.0, m_textTex->m_size.x, m_textTex->m_size.y};
    textBox.round(); // whole pixels, otherwise the text is sampled at a half-pixel offset and blurs

    g_pHyprOpenGL->renderTexture(m_textTex, textBox, {.a = a});
}

void CTitleDeco::renderGlow(PHLWINDOW w, PHLMONITOR pMonitor, const CBox& pillBox, float a) {
    // the same checks the window's own shadow makes, so the pill glows exactly when the
    // window does: inactive windows with a transparent shadow color get none
    const auto GLOW = glowConfig();
    if (GLOW.range <= 0 || w->m_ruleApplicator->noShadow().valueOrDefault())
        return;

    const bool FADING = w->m_shadowFadeAnimationProgress && w->m_shadowFadeAnimationProgress->isBeingAnimated();
    if (!hasAlpha(w->m_realShadowColor) && !(FADING && hasAlpha(w->m_realShadowColorPrevious)))
        return;

    const float SCALE = pMonitor->m_scale;
    const int   RANGE = std::round(GLOW.range * SCALE);

    CBox        box = pillBox;
    box.expand(RANGE).scaleFromCenter(GLOW.scale).translate(GLOW.offset * SCALE).round();
    if (box.w < 1 || box.h < 1)
        return;

    auto grad = w->m_realShadowColor;
    if (w->m_shadowAngleAnimationProgress && w->m_shadowAngleAnimationProgress->enabled()) {
        grad.m_angle = normalizeAngleRad(grad.m_angle + w->m_shadowAngleAnimationProgress->value() * M_PI * 2);
        if (FADING)
            w->m_realShadowColorPrevious.m_angle = grad.m_angle;
    }

    // with currentWindow set, the shadow shader cuts the window out, so the glow stays
    // outside the window like the window's own does and doesn't tint its content
    auto&      renderData = g_pHyprRenderer->m_renderData;
    const auto PREV       = renderData.currentWindow;
    renderData.currentWindow = m_window;

    const int ROUND = std::floor(pillBox.h / 2.0);
    if (FADING)
        g_pHyprRenderer->drawShadow(box, ROUND, 2.F, RANGE, w->m_realShadowColorPrevious, grad, w->m_shadowFadeAnimationProgress->value(), a);
    else
        g_pHyprRenderer->drawShadow(box, ROUND, 2.F, RANGE, grad, a);

    renderData.currentWindow = PREV;
}
