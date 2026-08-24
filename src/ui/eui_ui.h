// eui_ui.h — eui_neo.h minus the app-entry machinery (eui/detail/dsl_app_impl.h).
//
// Since eui-neo 0.5.6, eui_neo.h no longer includes dsl_app_impl.h at all — the
// app::update/render machinery moved into the app-main feature's own TU
// (glfw_app_main.cpp), so the mangled-name clash this header once avoided is
// gone. It is kept purely as a minimal include surface for the tinynext.ui.*
// modules (and app.cpp), which only need the widget/DslAppConfig declarations.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "eui/dsl_app.h"
#include "eui/dsl.h"
#include "eui/image.h"
#include "eui/json.h"
#include "eui/network.h"
#include "eui/platform.h"
#include "eui/signal.h"
#include "eui/types.h"
#include "components/components.h"
