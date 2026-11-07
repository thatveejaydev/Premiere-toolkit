#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "SPBasic.h"
#include "PrSDKAESupport.h"
#include "entry.h"

#include "BevelRender.h"
#include "PluginInfo.h"

namespace {

enum Parameter {
    Input,
    Thickness,
    Angle,
    Color,
    Intensity,
    Highlight,
    Shadow,
    ParameterCount
};

enum DiskID {
    ThicknessID = 1,
    AngleID,
    ColorID,
    IntensityID,
    HighlightID,
    ShadowID
};

constexpr bevel::Settings defaults{};
static_assert(BEVEL_VERSION == PF_VERSION(1, 3, 0, PF_Stage_RELEASE, 0));
static_assert(BEVEL_OUT_FLAGS == PF_OutFlag_DEEP_COLOR_AWARE);

template<class Channel>
struct BgraPixel {
    Channel blue, green, red, alpha;
};

class PremierePixelFormats {
public:
    explicit PremierePixelFormats(SPBasicSuite* basic) : basic_(basic) {
        const void* suite = nullptr;
        if (basic_ && !basic_->AcquireSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, &suite)) {
            suite_ = static_cast<const PF_PixelFormatSuite1*>(suite);
        }
    }

    ~PremierePixelFormats() {
        if (suite_) basic_->ReleaseSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
    }

    const PF_PixelFormatSuite1* get() const { return suite_; }

private:
    SPBasicSuite* basic_;
    const PF_PixelFormatSuite1* suite_ = nullptr;
};

PF_Err globalSetup(PF_InData* in_data, PF_OutData* out_data) {
    out_data->my_version = BEVEL_VERSION;
    out_data->out_flags = BEVEL_OUT_FLAGS;
    out_data->out_flags2 = BEVEL_OUT_FLAGS_2;
    if (in_data->appl_id == kAppID_Premiere) {
        const PremierePixelFormats formats(in_data->pica_basicP);
        if (const auto* suite = formats.get()) {
            if (const auto error = suite->ClearSupportedPixelFormats(in_data->effect_ref)) return error;
            if (const auto error = suite->AddSupportedPixelFormat(
                    in_data->effect_ref, PrPixelFormat_BGRA_4444_32f)) return error;
            if (const auto error = suite->AddSupportedPixelFormat(
                    in_data->effect_ref, PrPixelFormat_BGRA_4444_8u)) return error;
            out_data->out_flags2 |= PF_OutFlag2_FLOAT_COLOR_AWARE;
        }
    }
    return PF_Err_NONE;
}

PF_Err paramsSetup(PF_InData* in_data, PF_OutData* out_data) {
    PF_ParamDef def{};
    PF_ADD_FLOAT_SLIDERX("Edge Thickness", 0, 256, 0, 64, defaults.thickness,
                         PF_Precision_HUNDREDTHS, 0, 0, ThicknessID);
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Light Angle", static_cast<int>(defaults.angle), AngleID);
    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Light Color", 255, 255, 255, ColorID);
    PF_ADD_FLOAT_SLIDERX("Light Intensity", 0, 1, 0, 1, defaults.intensity,
                         PF_Precision_HUNDREDTHS, 0, 0, IntensityID);
    PF_ADD_FLOAT_SLIDERX("Highlight Strength", 0, 200, 0, 200, defaults.highlight * 100,
                         PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT,
                         PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS, HighlightID);
    PF_ADD_FLOAT_SLIDERX("Shadow Strength", 0, 200, 0, 200, defaults.shadow * 100,
                         PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT,
                         PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS, ShadowID);
    out_data->num_params = ParameterCount;
    return PF_Err_NONE;
}

float sampleSpacing(PF_RationalScale scale) {
    if (scale.num > 0 && scale.den > 0) {
        return static_cast<float>(scale.den) / static_cast<float>(scale.num);
    }
    return 1.0f;
}

template<class Pixel, unsigned MaxChannel, bool Premultiplied = true>
PF_Err renderFrame(PF_InData* in_data, const PF_LayerDef& source,
                   PF_LayerDef& output, bevel::Settings settings) {
    return bevel::render<MaxChannel, Premultiplied>(
        bevel::Frame<const Pixel>{reinterpret_cast<const Pixel*>(source.data),
            static_cast<int>(source.width), static_cast<int>(source.height), source.rowbytes},
        bevel::Frame<Pixel>{reinterpret_cast<Pixel*>(output.data),
            static_cast<int>(output.width), static_cast<int>(output.height), output.rowbytes},
        settings, sampleSpacing(in_data->downsample_x), sampleSpacing(in_data->downsample_y),
        [&] { return PF_ABORT(in_data); });
}

PF_Err render(PF_InData* in_data, PF_ParamDef* params[], PF_LayerDef& output) {
    auto& source = params[Input]->u.ld;
    if (output.width <= 0 || output.height <= 0) return PF_Err_NONE;
    if (!output.data || (!source.data && source.width > 0 && source.height > 0)) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    const PF_Pixel color = params[Color]->u.cd.value;
    constexpr float colorScale = 1.0f / PF_MAX_CHAN8;
    const double thickness = params[Thickness]->u.fs_d.value;
    const double intensity = params[Intensity]->u.fs_d.value;
    const auto strength = [&](Parameter index, float fallback) {
        if (!params[index]) return fallback;
        const double percent = params[index]->u.fs_d.value;
        return std::isfinite(percent) ? static_cast<float>(std::clamp(percent, 0.0, 200.0)) * 0.01f :
            fallback;
    };
    const bevel::Settings settings{
        std::isfinite(thickness) ? static_cast<float>(std::clamp(thickness, 0.0, 256.0)) : 0.0f,
        static_cast<float>(params[Angle]->u.ad.value) / 65536.0f,
        std::isfinite(intensity) ? static_cast<float>(std::clamp(intensity, 0.0, 1.0)) : 0.0f,
        color.red * colorScale, color.green * colorScale, color.blue * colorScale,
        strength(Highlight, defaults.highlight), strength(Shadow, defaults.shadow)
    };

    if (in_data->appl_id == kAppID_Premiere) {
        const PremierePixelFormats formats(in_data->pica_basicP);
        if (const auto* suite = formats.get()) {
            PrPixelFormat format = PrPixelFormat_Invalid;
            if (const auto error = suite->GetPixelFormat(&output, &format)) return error;
            if (source.data) {
                PrPixelFormat inputFormat = PrPixelFormat_Invalid;
                if (const auto error = suite->GetPixelFormat(&source, &inputFormat)) return error;
                if (inputFormat != format) return PF_Err_BAD_CALLBACK_PARAM;
            }
            switch (format) {
            case PrPixelFormat_BGRA_4444_32f:
                return renderFrame<BgraPixel<float>, 1, false>(in_data, source, output, settings);
            case PrPixelFormat_BGRA_4444_8u:
                return renderFrame<BgraPixel<A_u_char>, PF_MAX_CHAN8, false>(in_data, source, output, settings);
            case PrPixelFormat_ARGB_4444_8u:
                return renderFrame<PF_Pixel8, PF_MAX_CHAN8>(in_data, source, output, settings);
            default:
                return PF_Err_BAD_CALLBACK_PARAM;
            }
        }
    }

    if (PF_WORLD_IS_DEEP(&output)) {
        return renderFrame<PF_Pixel16, PF_MAX_CHAN16>(in_data, source, output, settings);
    }
    return renderFrame<PF_Pixel8, PF_MAX_CHAN8>(in_data, source, output, settings);
}

} // namespace

extern "C" __declspec(dllexport) PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite*, const char*, const char*) {
    PF_Err result = PF_Err_INVALID_CALLBACK;
    PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, BEVEL_NAME,
                           BEVEL_MATCH_NAME, BEVEL_CATEGORY, AE_RESERVED_INFO,
                           "EffectMain", "");
    return result;
}

extern "C" __declspec(dllexport) PF_Err EffectMain(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output, void*) {
    try {
        switch (cmd) {
        case PF_Cmd_ABOUT:
            std::snprintf(out_data->return_msg, sizeof(out_data->return_msg),
                          "Bevel Alpha 1.3.0\rDirectional lighting along alpha boundaries.");
            break;
        case PF_Cmd_GLOBAL_SETUP:
            return globalSetup(in_data, out_data);
        case PF_Cmd_PARAMS_SETUP:
            return paramsSetup(in_data, out_data);
        case PF_Cmd_FRAME_SETUP:
            out_data->width = params[Input]->u.ld.width;
            out_data->height = params[Input]->u.ld.height;
            out_data->origin.h = 0;
            out_data->origin.v = 0;
            break;
        case PF_Cmd_RENDER:
            return render(in_data, params, *output);
        default:
            break;
        }
    } catch (const std::bad_alloc&) {
        return PF_Err_OUT_OF_MEMORY;
    } catch (const std::length_error&) {
        return PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return PF_Err_NONE;
}
