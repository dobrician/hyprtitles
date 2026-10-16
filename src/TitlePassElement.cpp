#include "TitlePassElement.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include "TitleDeco.hpp"

CTitlePassElement::CTitlePassElement(const CTitlePassElement::STitleData& data_) : data(data_) {
    ;
}

std::vector<UP<IPassElement>> CTitlePassElement::draw() {
    data.deco->renderPass(g_pHyprRenderer->m_renderData.pMonitor.lock(), data.a);
    return {};
}

bool CTitlePassElement::needsLiveBlur() {
    return false;
}

bool CTitlePassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> CTitlePassElement::boundingBox() {
    const auto PWINDOW  = data.deco->getOwner();
    const auto PMONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!PWINDOW || !PMONITOR)
        return std::nullopt;

    // no texture yet means no size yet; returning nothing keeps the element from being
    // culled before its first draw, which is where the texture gets made
    auto box = data.deco->pillBoxGlobal(PWINDOW);
    if (box.w <= 0 || box.h <= 0)
        return std::nullopt;

    return box.translate(PWINDOW->m_floatingOffset - PMONITOR->m_position).expand(2);
}
