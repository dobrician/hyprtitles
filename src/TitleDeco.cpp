#include "TitleDeco.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRule.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <cmath>

#include <cairo/cairo.h>

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

static CHyprColor opaque(CHyprColor c) {
    c.a = 1.F;
    return c;
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

SPlacement placement() {
    // "top_right", "left bottom", "bottom-center", ...: the edge, then where along it
    auto pos = g_pGlobalState->config.position->value();
    std::ranges::replace(pos, '-', '_');
    std::ranges::replace(pos, ' ', '_');

    SPlacement p;
    if (pos.empty()) {
        // before `position` there was only `align`, on the top edge
        const auto ALIGN = g_pGlobalState->config.align->value();
        p.along          = ALIGN == "left" ? ALONG_START : ALIGN == "center" ? ALONG_CENTER : ALONG_END;
        return p;
    }

    const auto SEP  = pos.find('_');
    const auto EDGE = pos.substr(0, SEP);
    const auto AT   = SEP == std::string::npos ? std::string{"center"} : pos.substr(SEP + 1);

    p.edge = EDGE == "bottom" ? EDGE_BOTTOM : EDGE == "left" ? EDGE_LEFT : EDGE == "right" ? EDGE_RIGHT : EDGE_TOP;
    if (p.vertical())
        p.along = AT == "top" ? ALONG_START : AT == "bottom" ? ALONG_END : ALONG_CENTER;
    else
        p.along = AT == "left" ? ALONG_START : AT == "right" ? ALONG_END : ALONG_CENTER;
    return p;
}

// The label's box for a window box `wb` (logical px). `len` runs along the edge and
// `thick` across it. Outside, the label is centered on the border line, shifted inward
// by offset_y; inside, it hangs from the border's inner edge.
static CBox placeBox(const CBox& wb, double border, double rounding, double len, double thick, const SPlacement& p, bool inside) {
    const double OFFALONG  = std::max<Config::INTEGER>(0, g_pGlobalState->config.offsetX->value());
    const double OFFACROSS = g_pGlobalState->config.offsetY->value();
    const double SPAN      = p.vertical() ? wb.h : wb.w;
    const double ORIGIN    = p.vertical() ? wb.y : wb.x;

    double       along = 0;
    switch (p.along) {
        case ALONG_START: along = ORIGIN + rounding + OFFALONG; break;
        case ALONG_END: along = ORIGIN + SPAN - rounding - OFFALONG - len; break;
        case ALONG_CENTER: along = ORIGIN + (SPAN - len) / 2.0; break;
    }

    double across = 0;
    switch (p.edge) {
        case EDGE_TOP: across = inside ? wb.y : wb.y - border / 2.0 - thick / 2.0 + OFFACROSS; break;
        case EDGE_BOTTOM: across = inside ? wb.y + wb.h - thick : wb.y + wb.h + border / 2.0 - thick / 2.0 - OFFACROSS; break;
        case EDGE_LEFT: across = inside ? wb.x : wb.x - border / 2.0 - thick / 2.0 + OFFACROSS; break;
        case EDGE_RIGHT: across = inside ? wb.x + wb.w - thick : wb.x + wb.w + border / 2.0 - thick / 2.0 - OFFACROSS; break;
    }

    return p.vertical() ? CBox{across, along, thick, len} : CBox{along, across, len, thick};
}

SDecorationPositioningInfo CTitleDeco::getPositioningInfo() {
    // Absolute and non-solid: the pill overlaps the border and reserves no space, so the
    // layout and the other decorations stay where they are. The extents only widen the
    // window's damage box so the part of the pill past the border gets repainted.
    SDecorationPositioningInfo info;
    info.policy   = DECORATION_POSITION_ABSOLUTE;
    info.reserved = false;

    const double O = overhang();
    switch (placement().edge) {
        case EDGE_TOP:
            info.edges          = DECORATION_EDGE_TOP;
            info.desiredExtents = {Vector2D{0.0, O}, Vector2D{0.0, 0.0}};
            break;
        case EDGE_BOTTOM:
            info.edges          = DECORATION_EDGE_BOTTOM;
            info.desiredExtents = {Vector2D{0.0, 0.0}, Vector2D{0.0, O}};
            break;
        case EDGE_LEFT:
            info.edges          = DECORATION_EDGE_LEFT;
            info.desiredExtents = {Vector2D{O, 0.0}, Vector2D{0.0, 0.0}};
            break;
        case EDGE_RIGHT:
            info.edges          = DECORATION_EDGE_RIGHT;
            info.desiredExtents = {Vector2D{0.0, 0.0}, Vector2D{O, 0.0}};
            break;
    }
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

    // the tab's inverted corners reach out by at most half its thickness on each side
    const auto BOX = pillBoxGlobal(PWINDOW);
    if (BOX.w > 0 && BOX.h > 0)
        g_pHyprRenderer->damageBox(BOX.copy().expand(2 + std::max(m_inside ? std::min(BOX.w, BOX.h) / 2.0 : 0.0, glowReach())));
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
    m_tabTex       = nullptr;
    m_tabKey       = {};
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

    // follow the border, including its active <-> inactive fade, but opaque: themes often
    // give the inactive border some transparency, which on a pill leaves the text floating
    // over whatever is behind it
    const auto CURRENT = opaque(firstColor(w->m_realBorderColor));
    if (w->m_borderFadeAnimationProgress && w->m_borderFadeAnimationProgress->isBeingAnimated())
        return lerpColor(opaque(firstColor(w->m_realBorderColorPrevious)), CURRENT, w->m_borderFadeAnimationProgress->value());

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
    // logical px available for the text: the window's edge minus the rounded corners,
    // the offset from the corner and the pill padding, capped by max_width
    const auto   WINBOX   = w->getWindowMainSurfaceBox();
    const double ROUNDING = w->rounding();
    const double OFFSET   = std::max<Config::INTEGER>(0, g_pGlobalState->config.offsetX->value());
    const double PADDING  = std::max<Config::INTEGER>(0, g_pGlobalState->config.paddingX->value());
    const double SPAN     = placement().vertical() ? WINBOX.h : WINBOX.w;
    double       avail    = SPAN - 2 * ROUNDING - 2 * OFFSET - 2 * PADDING;

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
    // how far the pill reaches past the window's main surface on its edge (logical px)
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

    const double SCALE = m_texKey.scale;
    const double PADX  = std::max<Config::INTEGER>(0, g_pGlobalState->config.paddingX->value());
    const double PADY  = std::max<Config::INTEGER>(0, g_pGlobalState->config.paddingY->value());
    const double LEN   = m_textTex->m_size.x / SCALE + 2 * PADX;
    const double THICK = m_textTex->m_size.y / SCALE + 2 * PADY;

    CBox         box = placeBox(w->getWindowMainSurfaceBox(), w->getRealBorderSize(), w->rounding(), LEN, THICK, placement(), m_inside);

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

    if (const bool INSIDE = wantsInside(PWINDOW); INSIDE != m_inside) {
        damageEntire();
        m_inside = INSIDE;
        damageEntire();
    }

    CBox box = pillBoxGlobal(PWINDOW);
    box.translate(PWINDOW->m_floatingOffset - pMonitor->m_position).scale(SCALE).round();

    if (box.w < 1 || box.h < 1)
        return;

    const auto   PLACE = placement();
    const double THICK = PLACE.vertical() ? box.w : box.h;

    if (m_inside) {
        // free corners like the window's, capped so short tabs stay tabs
        const int R = std::round(std::min(std::max<double>(PWINDOW->rounding(), 3.0), THICK / 2.0 / SCALE) * SCALE);
        renderGlow(PWINDOW, pMonitor, box, R, a);
        renderTab(box, PLACE, backgroundColor(PWINDOW), R, a);
    } else {
        renderGlow(PWINDOW, pMonitor, box, std::floor(THICK / 2.0), a);

        CHyprColor bg = backgroundColor(PWINDOW);
        bg.a *= a;

        g_pHyprOpenGL->renderRect(box, bg, {.round = static_cast<int>(std::floor(THICK / 2.0)), .roundingPower = 2.F});
    }

    // on the side edges the text runs along the edge, read from the outside of the
    // window: bottom to top on the left, top to bottom on the right
    const auto TEXT    = m_textTex->m_size;
    const auto TEXTDIM = PLACE.vertical() ? Vector2D{TEXT.y, TEXT.x} : TEXT;
    m_textTex->m_transform = PLACE.edge == EDGE_LEFT ? HYPRUTILS_TRANSFORM_90 : PLACE.edge == EDGE_RIGHT ? HYPRUTILS_TRANSFORM_270 : HYPRUTILS_TRANSFORM_NORMAL;

    CBox textBox = {box.x + (box.w - TEXTDIM.x) / 2.0, box.y + (box.h - TEXTDIM.y) / 2.0, TEXTDIM.x, TEXTDIM.y};
    textBox.round(); // whole pixels, otherwise the text is sampled at a half-pixel offset and blurs

    g_pHyprOpenGL->renderTexture(m_textTex, textBox, {.a = a});
}

void CTitleDeco::renderGlow(PHLWINDOW w, PHLMONITOR pMonitor, const CBox& pillBox, int round, float a) {
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

    // inside, the glow falls into the window straight down from the tab, so the
    // shadow's offset and scale, meant for the outside, don't apply
    CBox box = pillBox;
    box.expand(RANGE);
    if (!m_inside)
        box.scaleFromCenter(GLOW.scale).translate(GLOW.offset * SCALE);
    box.round();
    if (box.w < 1 || box.h < 1)
        return;

    auto grad = w->m_realShadowColor;
    if (w->m_shadowAngleAnimationProgress && w->m_shadowAngleAnimationProgress->enabled()) {
        grad.m_angle = normalizeAngleRad(grad.m_angle + w->m_shadowAngleAnimationProgress->value() * M_PI * 2);
        if (FADING)
            w->m_realShadowColorPrevious.m_angle = grad.m_angle;
    }

    // Outside, currentWindow makes the shadow shader cut the window out, so the glow stays
    // outside like the window's own does. Inside it is the reverse: clipped to the window.
    auto&      renderData = g_pHyprRenderer->m_renderData;
    const auto PREVWINDOW = renderData.currentWindow;
    const auto PREVCLIP   = renderData.clipBox;

    if (m_inside) {
        CBox       clip       = w->getWindowMainSurfaceBox();
        const auto PWORKSPACE = w->m_workspace;
        if (PWORKSPACE && !w->m_pinned)
            clip.translate(PWORKSPACE->m_renderOffset->value());
        clip.translate(w->m_floatingOffset - pMonitor->m_position).scale(SCALE).round();
        if (PREVCLIP.width != 0 && PREVCLIP.height != 0)
            clip = clip.intersection(PREVCLIP);
        if (clip.w < 1 || clip.h < 1)
            return;
        renderData.clipBox = clip;
    } else
        renderData.currentWindow = m_window;

    if (FADING)
        g_pHyprRenderer->drawShadow(box, round, 2.F, RANGE, w->m_realShadowColorPrevious, grad, w->m_shadowFadeAnimationProgress->value(), a);
    else
        g_pHyprRenderer->drawShadow(box, round, 2.F, RANGE, grad, a);

    renderData.currentWindow = PREVWINDOW;
    renderData.clipBox       = PREVCLIP;
}

bool CTitleDeco::wantsInside(PHLWINDOW w) {
    const auto MODE = g_pGlobalState->config.inside->value();
    if (MODE == "always")
        return true;
    if (MODE != "auto")
        return false;

    const auto PMONITOR = w->m_monitor.lock();
    if (!PMONITOR || !m_textTex || m_texKey.scale <= 0)
        return false;

    // Decide on where the window is going, not where it is mid-animation, so the label
    // doesn't flip between the two places while a window slides or resizes.
    const auto   GOAL  = w->geometricBox(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    const auto   PLACE = placement();
    const double PADX  = std::max<Config::INTEGER>(0, g_pGlobalState->config.paddingX->value());
    const double PADY  = std::max<Config::INTEGER>(0, g_pGlobalState->config.paddingY->value());
    const double LEN   = m_textTex->m_size.x / m_texKey.scale + 2 * PADX;
    const double THICK = m_textTex->m_size.y / m_texKey.scale + 2 * PADY;
    const auto   OUT   = placeBox(GOAL, w->getRealBorderSize(), w->rounding(), LEN, THICK, PLACE, false);

    // past the usable area on that side: off screen, or over a bar
    const auto WORK = PMONITOR->logicalBoxMinusReserved();
    switch (PLACE.edge) {
        case EDGE_TOP:
            if (OUT.y < WORK.y)
                return true;
            break;
        case EDGE_BOTTOM:
            if (OUT.y + OUT.h > WORK.y + WORK.h)
                return true;
            break;
        case EDGE_LEFT:
            if (OUT.x < WORK.x)
                return true;
            break;
        case EDGE_RIGHT:
            if (OUT.x + OUT.w > WORK.x + WORK.w)
                return true;
            break;
    }

    if (w->m_isFloating)
        return false;

    // over a tiled neighbour on that side (gaps smaller than half the pill); tiled
    // windows don't overlap, so whatever the label would cover is on its side
    for (const auto& other : Desktop::windowState()->windows()) {
        if (other == w || !other->m_isMapped || other->isHidden() || other->m_isFloating || other->m_workspace != w->m_workspace)
            continue;

        const auto OB = other->geometricBox(Desktop::View::IGeometric::GEOMETRIC_GOAL).expand(other->getRealBorderSize());
        if (const auto HIT = OB.intersection(OUT); HIT.w > 0 && HIT.h > 0)
            return true;
    }

    return false;
}

void CTitleDeco::renderTab(const CBox& body, const SPlacement& place, const CHyprColor& color, int r, float a) {
    // The tab hangs from the border: flat where it meets it, rounded at the free end, and
    // joined to the border by inverted corners of the same radius on either side. Drawn
    // with cairo hanging from a top border, once per size and color (only a border color
    // fade redraws it per frame), and turned to the label's edge when rendered.
    const bool VERT = place.vertical();
    const int  W    = std::round(VERT ? body.h : body.w) + 2 * r;
    const int  H    = std::round(VERT ? body.w : body.h);
    if (W < 1 || H < 1)
        return;

    if (!m_tabTex || m_tabKey.w != W || m_tabKey.h != H || m_tabKey.r != r || m_tabKey.color != color) {
        m_tabKey = {.w = W, .h = H, .r = r, .color = color};

        auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        auto* cr      = cairo_create(surface);

        const double RR = std::min<double>(r, H / 2.0);
        const double X0 = r, X1 = W - r;

        cairo_move_to(cr, 0, 0);
        cairo_line_to(cr, W, 0);
        cairo_arc_negative(cr, W, r, r, -M_PI / 2.0, M_PI); // inverted corner, right
        cairo_line_to(cr, X1, H - RR);
        cairo_arc(cr, X1 - RR, H - RR, RR, 0, M_PI / 2.0);
        cairo_line_to(cr, X0 + RR, H);
        cairo_arc(cr, X0 + RR, H - RR, RR, M_PI / 2.0, M_PI);
        cairo_line_to(cr, X0, r);
        cairo_arc_negative(cr, 0, r, r, 0, -M_PI / 2.0); // inverted corner, left
        cairo_close_path(cr);

        cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
        cairo_fill(cr);
        cairo_surface_flush(surface);

        m_tabTex = g_pHyprRenderer->createTexture(surface);

        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }

    if (!m_tabTex || m_tabTex->m_texID == 0)
        return;

    switch (place.edge) {
        case EDGE_TOP: m_tabTex->m_transform = HYPRUTILS_TRANSFORM_NORMAL; break;
        case EDGE_BOTTOM: m_tabTex->m_transform = HYPRUTILS_TRANSFORM_180; break;
        case EDGE_LEFT: m_tabTex->m_transform = HYPRUTILS_TRANSFORM_90; break;
        case EDGE_RIGHT: m_tabTex->m_transform = HYPRUTILS_TRANSFORM_270; break;
    }

    const CBox DEST = VERT ? CBox{body.x, body.y - r, (double)H, (double)W} : CBox{body.x - r, body.y, (double)W, (double)H};
    g_pHyprOpenGL->renderTexture(m_tabTex, DEST, {.a = a});
}
