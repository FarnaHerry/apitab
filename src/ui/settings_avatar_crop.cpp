// settings_avatar_crop.cpp — 头像裁剪子系统（自 settings_page.cpp 拆出，
// 功能域 = 裁剪对话框）：拖放/滚轮接入、仿射变换数学、缩放控制条与裁剪
// 弹窗。PersonalInfoSettingsSection（settings_page.cpp）只经 ui.h 声明使用
// AvatarCropImage / AvatarCropDialog；其余变换与裁剪 helpers 为本 TU 私有。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "ui.h"
#include "task_bridge.h"

import apitab.config;
import apitab.preferences;

// 应用版本：CMake 注入（CMakeLists.txt 的 target_compile_definitions
// APITAB_VERSION="${PROJECT_VERSION}"，随顶层 project(VERSION) 自动同步）；
// 未注入（如脱离构建系统的单文件检查）时兜底 "dev"。
#ifndef APITAB_VERSION
#define APITAB_VERSION "dev"
#endif

namespace apitab::ui {

namespace {
std::string ShellQuote(const std::string& value) {
    std::string quoted{"'"};
    for (const char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += '\'';
    return quoted;
}

float ClampCropValue(float value, float lower, float upper) {
    if (upper < lower) std::swap(lower, upper);
    return std::clamp(value, lower, upper);
}

// HCG 不允许在 [[composable]] 函数体内使用条件编译，因此 SDK 版本差异
// 收敛在普通 C++ 辅助函数中。离线 0.2.0 SDK 没有这些事件时仍保留点击选图和
// 触控板变换手势。
template <typename ClampOffset>
huxerui::View WithCropScrollZoom(huxerui::View stage, huxerui::State<float> imageScale,
                                 huxerui::State<float> imageRotation,
                                 huxerui::State<float> imageOffsetX,
                                 huxerui::State<float> imageOffsetY, ClampOffset clampOffset) {
#if defined(APITAB_HAS_SCROLL_INPUT)
    return std::move(stage).On<huxerui::ViewEvents::ScrollInput>(
        [imageScale, imageRotation, imageOffsetX, imageOffsetY, clampOffset](
            const huxerui::ScrollInputEvent& event) {
            const float nextScale = ClampCropValue(
                imageScale.Get() * std::exp(-event.delta_y * 0.002F), 1.0F, 8.0F);
            const huxerui::Point offset = clampOffset(
                nextScale, imageRotation.Get(), {imageOffsetX.Get(), imageOffsetY.Get()});
            imageScale = nextScale;
            imageOffsetX = offset.x;
            imageOffsetY = offset.y;
            return true;
        });
#else
    (void)imageScale;
    (void)imageRotation;
    (void)imageOffsetX;
    (void)imageOffsetY;
    (void)clampOffset;
    return stage;
#endif
}

huxerui::Transform2D MultiplyTransform(const huxerui::Transform2D& outer,
                                        const huxerui::Transform2D& inner) {
    return {
        outer.m11 * inner.m11 + outer.m21 * inner.m12,
        outer.m12 * inner.m11 + outer.m22 * inner.m12,
        outer.m11 * inner.m21 + outer.m21 * inner.m22,
        outer.m12 * inner.m21 + outer.m22 * inner.m22,
        outer.m11 * inner.translate_x + outer.m21 * inner.translate_y + outer.translate_x,
        outer.m12 * inner.translate_x + outer.m22 * inner.translate_y + outer.translate_y,
    };
}

huxerui::Transform2D CropUserTransform(float scale, float rotationDegrees,
                                       huxerui::Point offset, float stageSize) {
    constexpr float kPi = 3.14159265358979323846F;
    const float radians = rotationDegrees * kPi / 180.0F;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const huxerui::Point center{stageSize / 2.0F, stageSize / 2.0F};
    const huxerui::Transform2D translateOffset{1.0F, 0.0F, 0.0F, 1.0F,
                                               offset.x, offset.y};
    const huxerui::Transform2D translateCenter{1.0F, 0.0F, 0.0F, 1.0F,
                                                center.x, center.y};
    const huxerui::Transform2D rotate{cosine, sine, -sine, cosine};
    const huxerui::Transform2D scaleTransform{scale, 0.0F, 0.0F, scale};
    const huxerui::Transform2D translateBack{1.0F, 0.0F, 0.0F, 1.0F,
                                             -center.x, -center.y};
    return MultiplyTransform(
        translateOffset,
        MultiplyTransform(translateCenter,
                          MultiplyTransform(rotate,
                                            MultiplyTransform(scaleTransform, translateBack))));
}

huxerui::Transform2D CropSourceTransform(float scale, float rotationDegrees,
                                         huxerui::Point offset, float imageLeft,
                                         float imageTop, float fitScale,
                                         float stageSize) {
    const huxerui::Transform2D fitBase{fitScale, 0.0F, 0.0F, fitScale,
                                       imageLeft, imageTop};
    const huxerui::Transform2D user =
        CropUserTransform(scale, rotationDegrees, offset, stageSize);
    return MultiplyTransform(user, fitBase);
}

huxerui::Rect TransformedBounds(const huxerui::Transform2D& transform,
                                float rectLeft, float rectTop, float width, float height) {
    const huxerui::Point points[] = {{rectLeft, rectTop}, {rectLeft + width, rectTop},
                                     {rectLeft, rectTop + height},
                                     {rectLeft + width, rectTop + height}};
    float left = transform.Apply(points[0]).x;
    float right = left;
    float top = transform.Apply(points[0]).y;
    float bottom = top;
    for (int index = 1; index < 4; ++index) {
        const huxerui::Point point = points[index];
        const huxerui::Point transformed = transform.Apply(point);
        left = std::min(left, transformed.x);
        right = std::max(right, transformed.x);
        top = std::min(top, transformed.y);
        bottom = std::max(bottom, transformed.y);
    }
    return {left, top, right - left, bottom - top};
}

[[huxerui::composable]] huxerui::View AvatarCropZoomControls(
    huxerui::State<float> imageScale, huxerui::State<float> imageRotation,
    huxerui::State<float> imageOffsetX, huxerui::State<float> imageOffsetY,
    std::function<huxerui::Point(float, float, huxerui::Point)> clampOffset) {
    const auto& theme = huxerui::UseTheme();
    return huxerui::Row {
      huxerui::Text("图片缩放", huxerui::TextRole::Label),
      huxerui::Slider(imageScale)
          .Range(1.0F, 8.0F)
          .Step(0.01F)
          .With(huxerui::Grow(1.0F))
          .OnChanged([imageScale, imageRotation, imageOffsetX, imageOffsetY,
                      clampOffset](float value) {
              const float nextScale = ClampCropValue(value, 1.0F, 8.0F);
              const huxerui::Point offset = clampOffset(
                  nextScale, imageRotation.Get(),
                  {imageOffsetX.Get(), imageOffsetY.Get()});
              imageScale = nextScale;
              imageOffsetX = offset.x;
              imageOffsetY = offset.y;
          }),
      huxerui::Text(std::to_string(static_cast<int>(imageScale.Get() * 100.0F)) + "%",
                    huxerui::TextRole::Label)
          .With(huxerui::Frame{.width = 56.0F}),
    }.With(huxerui::Spacing(theme.spacing.small),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

struct AvatarCropOutput {
    std::filesystem::path path;
    huxerui::ImageAsset image;
    ~AvatarCropOutput() {
        std::error_code error;
        if (!path.empty()) std::filesystem::remove(path, error);
    }
};
} // namespace

// 单独的组合边界：拖动只更新图片变换，缩放只额外更新缩放条。
[[huxerui::composable]] huxerui::View AvatarCropImage(
    huxerui::ImageAsset source, float stageSize,
    huxerui::State<float> imageScale, huxerui::State<float> imageRotation,
    huxerui::State<float> imageOffsetX, huxerui::State<float> imageOffsetY) {
    if (!source.HasValue()) return huxerui::Text("无法预览", huxerui::TextRole::Body);
    return huxerui::Stack {
      huxerui::Image(source)
          .Fit(huxerui::ImageFit::Contain)
          .With(huxerui::Frame{.width = stageSize, .height = stageSize},
                huxerui::Scale(imageScale.Get()), huxerui::Rotation(imageRotation.Get())),
    }.With(huxerui::Frame{.width = stageSize, .height = stageSize},
           huxerui::Offset(huxerui::Point{imageOffsetX.Get(), imageOffsetY.Get()}));
}

[[huxerui::composable]] huxerui::View AvatarCropDialog(
    huxerui::DialogContext ctx, std::string sourcePath, huxerui::ImageAsset source,
    huxerui::State<std::string> avatarName,
    huxerui::State<huxerui::ImageAsset> avatarImage, std::shared_ptr<bool> canceled) {
    const auto& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto saving = huxerui::UseState(false);
    constexpr float kStageSize = 480.0F;
    const float sourceWidth = source.HasValue() ? static_cast<float>(source.PixelWidth()) : 1.0F;
    const float sourceHeight = source.HasValue() ? static_cast<float>(source.PixelHeight()) : 1.0F;
    const float fitScale = std::min(kStageSize / sourceWidth, kStageSize / sourceHeight);
    const float imageWidth = sourceWidth * fitScale;
    const float imageHeight = sourceHeight * fitScale;
    const float imageLeft = (kStageSize - imageWidth) / 2.0F;
    const float imageTop = (kStageSize - imageHeight) / 2.0F;
    const float maxSide = std::min(imageWidth, imageHeight);
    const float cropSide = maxSide;
    const float cropLeft = (kStageSize - cropSide) / 2.0F;
    const float cropTop = (kStageSize - cropSide) / 2.0F;
    auto imageScale = huxerui::UseState(1.0F);
    auto imageRotation = huxerui::UseState(0.0F);
    auto imageOffsetX = huxerui::UseState(0.0F);
    auto imageOffsetY = huxerui::UseState(0.0F);
    auto originOffsetX = huxerui::UseState(0.0F);
    auto originOffsetY = huxerui::UseState(0.0F);
    const std::string outputPath = (cfg::dataDir() / "avatar.png").string();
    const auto transformFor = [imageLeft, imageTop, fitScale, kStageSize](
                                  float scale, float rotation, huxerui::Point offset) {
        return CropSourceTransform(scale, rotation, offset, imageLeft, imageTop,
                                   fitScale, kStageSize);
    };
    const auto clampOffsetForCrop = [=](float side, float scale, float rotation,
                                        huxerui::Point desired) {
        const huxerui::Transform2D transform = transformFor(scale, rotation, {});
        const huxerui::Rect bounds = TransformedBounds(transform, 0.0F, 0.0F,
                                                       sourceWidth, sourceHeight);
        const float left = (kStageSize - side) / 2.0F;
        const float top = (kStageSize - side) / 2.0F;
        const float minX = left + side - bounds.x - bounds.width;
        const float maxX = left - bounds.x;
        const float minY = top + side - bounds.y - bounds.height;
        const float maxY = top - bounds.y;
        return huxerui::Point{ClampCropValue(desired.x, minX, maxX),
                              ClampCropValue(desired.y, minY, maxY)};
    };
    const auto clampOffset = [=](float scale, float rotation, huxerui::Point desired) {
        return clampOffsetForCrop(cropSide, scale, rotation, desired);
    };
    auto crop = [ctx, sourcePath, outputPath, avatarName, avatarImage, imageScale,
                 imageRotation, imageOffsetX, imageOffsetY, transformFor,
                 sourceWidth, sourceHeight, cropLeft, cropTop, cropSide, tasks, toast, saving, canceled] {
        if (saving.Get() || *canceled) return;
        const std::string sourceArg = ShellQuote(sourcePath);
        const int width = std::max(1, static_cast<int>(sourceWidth));
        const int height = std::max(1, static_cast<int>(sourceHeight));
        const huxerui::Transform2D transform = transformFor(
            imageScale.Get(), imageRotation.Get(),
            {imageOffsetX.Get(), imageOffsetY.Get()});
        const huxerui::Point cropPoints[] = {
            {cropLeft, cropTop}, {cropLeft + cropSide, cropTop},
            {cropLeft, cropTop + cropSide}, {cropLeft + cropSide, cropTop + cropSide}};
        const auto first = transform.Inverse(cropPoints[0]);
        if (!first) return;
        float left = first->x;
        float right = first->x;
        float top = first->y;
        float bottom = first->y;
        for (int index = 1; index < 4; ++index) {
            const huxerui::Point point = cropPoints[index];
            const auto sourcePoint = transform.Inverse(point);
            if (!sourcePoint) return;
            left = std::min(left, sourcePoint->x);
            right = std::max(right, sourcePoint->x);
            top = std::min(top, sourcePoint->y);
            bottom = std::max(bottom, sourcePoint->y);
        }
        const float square = std::min(right - left, bottom - top);
        if (!std::isfinite(square) || square <= 0.0F) return;
        const float centerX = (left + right) / 2.0F;
        const float centerY = (top + bottom) / 2.0F;
        const int side = std::max(1, std::min(
            static_cast<int>(square), std::min(width, height)));
        const int x = std::clamp(static_cast<int>(centerX - static_cast<float>(side) / 2.0F),
                                 0, width - side);
        const int y = std::clamp(static_cast<int>(centerY - static_cast<float>(side) / 2.0F),
                                 0, height - side);
        saving = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            try {
                auto result = co_await RunOnTaskThread([sourceArg, side, x, y, outputPath] {
                    auto output = std::make_shared<AvatarCropOutput>();
                    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
                    output->path = std::filesystem::path(outputPath).parent_path() /
                                   ("avatar-crop-" + std::to_string(stamp) + ".png");
                    const std::string outputArg = ShellQuote(output->path.string());
                    const std::string arguments = sourceArg + " -crop " + std::to_string(side) + "x" +
                        std::to_string(side) + "+" + std::to_string(x) + "+" + std::to_string(y) +
                        " +repage -resize 1024x1024 " + outputArg;
                    const std::string command =
                        "(command -v magick >/dev/null 2>&1 && magick " + arguments +
                        ") || (command -v convert >/dev/null 2>&1 && convert " + arguments + ")";
                    if (std::system(command.c_str()) != 0)
                        throw std::runtime_error("图片处理失败，请检查 ImageMagick 是否可用");
                    output->image = huxerui::ImageAsset::FromFile(output->path);
                    if (!output->image.HasValue()) throw std::runtime_error("裁剪结果无法读取");
                    return output;
                });
                // 关闭动画尚未卸载弹窗时也立即取消回写，后台结果仅清理临时文件。
                if (*canceled) co_return;
                std::filesystem::rename(result->path, outputPath);
                avatarImage = result->image;
                avatarName = "avatar.png";
                saveSessionPreference("profile_avatar_name", "avatar.png");
                saving = false;
                ctx.Dismiss();
            } catch (const std::exception& error) {
                if (*canceled) co_return;
                saving = false;
                toast.Show(std::string{"保存头像失败："} + error.what());
            }
        });
    };
    huxerui::View mask = huxerui::Canvas([cropLeft, cropTop, cropSide](huxerui::PaintContext& paint,
                                                                        huxerui::Size) {
        const huxerui::Color shade = huxerui::Color::Rgb(0, 0, 0, 0.58F);
        const huxerui::Point center{cropLeft + cropSide / 2.0F,
                                    cropTop + cropSide / 2.0F};
        const float radius = cropSide / 2.0F;
        huxerui::Path shaded;
        shaded.MoveTo({0.0F, 0.0F})
            .LineTo({kStageSize, 0.0F})
            .LineTo({kStageSize, kStageSize})
            .LineTo({0.0F, kStageSize})
            .Close();
        shaded.MoveTo({center.x + radius, center.y})
            .ArcTo({radius, radius}, 0.0F, huxerui::ArcSize::Small,
                   huxerui::ArcDirection::Clockwise, {center.x - radius, center.y})
            .ArcTo({radius, radius}, 0.0F, huxerui::ArcSize::Small,
                   huxerui::ArcDirection::Clockwise, {center.x + radius, center.y})
            .Close();
        paint.FillPath(shaded, shade, huxerui::PathFillRule::EvenOdd);
    });
    huxerui::View controls = huxerui::Canvas([cropLeft, cropTop, cropSide](huxerui::PaintContext& paint,
                                                                            huxerui::Size) {
        const huxerui::Color outline = huxerui::Color::Rgb(255, 255, 255, 0.95F);
        paint.DrawBorder({cropLeft, cropTop, cropSide, cropSide}, outline,
                         huxerui::StrokeStyle{.width = 1.0F});
        paint.DrawArc({cropLeft + cropSide / 2.0F, cropTop + cropSide / 2.0F}, cropSide / 2.0F,
                       0.0F,
                       6.2831853F, outline, huxerui::StrokeStyle{.width = 2.0F});
    });
    huxerui::View stage = huxerui::Stack {
        AvatarCropImage(source, kStageSize, imageScale, imageRotation, imageOffsetX, imageOffsetY),
        std::move(mask).With(huxerui::Frame{.width = kStageSize, .height = kStageSize}),
        std::move(controls).With(huxerui::Frame{.width = kStageSize, .height = kStageSize}),
    }.With(huxerui::Frame{.width = kStageSize, .height = kStageSize},
           huxerui::Background(theme.colors.surface_container_high), huxerui::ClipChildren(),
           huxerui::Enabled(!saving.Get()),
           huxerui::DragGesture{.minimum_distance = 0.0F}, huxerui::TransformGesture{})
        .On<huxerui::DragEvents::Started>(
            [imageOffsetX, imageOffsetY, originOffsetX, originOffsetY](const huxerui::DragEvent&) {
                originOffsetX = imageOffsetX.Get();
                originOffsetY = imageOffsetY.Get();
            })
        .On<huxerui::DragEvents::Changed>(
            [imageScale, imageRotation, imageOffsetX, imageOffsetY, originOffsetX,
             originOffsetY, clampOffset](const huxerui::DragEvent& event) {
                const huxerui::Point offset = clampOffset(
                    imageScale.Get(), imageRotation.Get(),
                    {originOffsetX.Get() + event.translation.x,
                     originOffsetY.Get() + event.translation.y});
                imageOffsetX = offset.x;
                imageOffsetY = offset.y;
            })
        .On<huxerui::TransformEvents::Changed>(
            [imageScale, imageRotation, imageOffsetX, imageOffsetY, clampOffset](
                const huxerui::TransformEvent& event) {
                if (event.pointer_count < 2) return;
                const float nextScale = ClampCropValue(imageScale.Get() * event.scale, 1.0F, 8.0F);
                const float nextRotation = imageRotation.Get() +
                    event.rotation * 180.0F / 3.14159265358979323846F;
                const huxerui::Point offset = clampOffset(
                    nextScale, nextRotation,
                    {imageOffsetX.Get() + event.pan.x, imageOffsetY.Get() + event.pan.y});
                imageScale = nextScale;
                imageRotation = nextRotation;
                imageOffsetX = offset.x;
                imageOffsetY = offset.y;
            });
    stage = WithCropScrollZoom(std::move(stage), imageScale, imageRotation, imageOffsetX,
                               imageOffsetY, clampOffset);
    return DialogCard(huxerui::Column {
        huxerui::Text("裁剪头像", huxerui::TextRole::Title),
        std::move(stage),
        huxerui::Text("拖动底图调整位置，滚轮缩放；触控板双指可缩放和旋转，中心圆形区域是头像预览。",
                      huxerui::TextRole::Body),
        AvatarCropZoomControls(imageScale, imageRotation, imageOffsetX, imageOffsetY, clampOffset)
            .With(huxerui::Enabled(!saving.Get())),
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx, canceled] { *canceled = true; ctx.Dismiss(); }),
            huxerui::Button(saving.Get() ? "正在保存…" : "设置新头像")
                .With(huxerui::Enabled(source.HasValue() && !saving.Get())).OnClick(crop),
        }.With(huxerui::Spacing(theme.spacing.small),
               huxerui::MainAlign(huxerui::MainAxisAlignment::End)),
    }.With(huxerui::Spacing(theme.spacing.medium), huxerui::Frame{.width = 520.0F},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}
} // namespace apitab::ui
