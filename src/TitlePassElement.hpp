#pragma once
#include <hyprland/src/render/pass/PassElement.hpp>

class CTitleDeco;

class CTitlePassElement : public IPassElement {
  public:
    struct STitleData {
        CTitleDeco* deco = nullptr;
        float       a    = 1.F;
    };

    CTitlePassElement(const STitleData& data_);
    virtual ~CTitlePassElement() = default;

    virtual std::vector<UP<IPassElement>> draw() override;
    virtual bool                          needsLiveBlur() override;
    virtual bool                          needsPrecomputeBlur() override;
    virtual std::optional<CBox>           boundingBox() override;

    virtual const char*                   passName() override {
        return "CTitlePassElement";
    }

    virtual ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    STitleData data;
};
