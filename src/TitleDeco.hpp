#pragma once

#define WLR_USE_UNSTABLE

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/helpers/Color.hpp>

#include <optional>
#include <string>

#include "globals.hpp"

// What the pill shows for one window, as resolved from the window title or the lua callback.
struct SLabel {
    bool                      hidden = false;
    std::string               text;
    std::optional<CHyprColor> bg, fg;

    bool                      operator==(const SLabel&) const = default;
};

enum eEdge : uint8_t {
    EDGE_TOP,
    EDGE_BOTTOM,
    EDGE_LEFT,
    EDGE_RIGHT,
};

enum eAlong : uint8_t {
    ALONG_START, // left on a horizontal edge, top on a vertical one
    ALONG_CENTER,
    ALONG_END,
};

// where the label sits: which edge of the window, and where along it
struct SPlacement {
    eEdge  edge  = EDGE_TOP;
    eAlong along = ALONG_END;

    bool   vertical() const {
        return edge == EDGE_LEFT || edge == EDGE_RIGHT;
    }
};

SPlacement placement();

// how far the pill glow reaches past the pill (logical px), 0 without a glow
double glowReach();

class CTitleDeco : public IHyprWindowDecoration {
  public:
    CTitleDeco(PHLWINDOW);
    virtual ~CTitleDeco();

    virtual SDecorationPositioningInfo getPositioningInfo();
    virtual void                       onPositioningReply(const SDecorationPositioningReply& reply);
    virtual void                       draw(PHLMONITOR, float const& a);
    virtual eDecorationType            getDecorationType();
    virtual void                       updateWindow(PHLWINDOW);
    virtual void                       damageEntire();
    virtual eDecorationLayer           getDecorationLayer();
    virtual uint64_t                   getDecorationFlags();
    virtual std::string                getDisplayName();

    PHLWINDOW                          getOwner();

    void                               setLabel(SLabel&& label);
    void                               updateRules();
    void                               onConfigReloaded();

    bool                               m_labelDirty = true;
    WP<CTitleDeco>                     m_self;

  private:
    PHLWINDOWREF         m_window;

    SLabel               m_label;
    bool                 m_noTitleRule = false;

    SP<Render::ITexture> m_textTex;
    bool                 m_textTruncated = false;

    // what m_textTex was rendered with; any change re-renders it
    struct {
        std::string text, font;
        CHyprColor  color;
        float       scale     = 0;
        int         size      = 0;
        int         weight    = 0;
        int         maxWidthPx = 0;
    } m_texKey;

    double     m_lastOverhang = -1;

    // inside: the label hangs from the top border into the window as a tab, used when
    // there is no room above the window (no gaps, or a bar or another window right above)
    bool                 m_inside = false;
    SP<Render::ITexture> m_tabTex;
    struct {
        int        w = 0, h = 0, r = 0;
        CHyprColor color;
    } m_tabKey;

    bool       shouldShow(PHLWINDOW w);
    bool       isActive(PHLWINDOW w);
    CHyprColor backgroundColor(PHLWINDOW w);
    CHyprColor textColor(PHLWINDOW w);
    int        maxTextWidth(PHLWINDOW w);
    void       ensureTexture(PHLWINDOW w, float scale);
    CBox       pillBoxGlobal(PHLWINDOW w);
    bool       wantsInside(PHLWINDOW w);
    void       renderTab(const CBox& body, const SPlacement& place, const CHyprColor& color, int r, float a);
    double     overhang();

    void       renderPass(PHLMONITOR, float const& a);
    void       renderGlow(PHLWINDOW w, PHLMONITOR pMonitor, const CBox& pillBox, int round, float a);

    friend class CTitlePassElement;
};
