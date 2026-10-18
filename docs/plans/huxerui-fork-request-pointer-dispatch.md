# HuxerUI fork 需求:跨窗口拖拽的指针序列注入 API

> 提出方:apitab 多窗口拖回(Chrome 式标签实时合并)。
> 交接日期:2026-10-18。目标分支:`FarnaHerry/HuxerUI` `farna/main`(基线 fa04cb4)。

## 背景与用途

apitab 正在做 Chrome 式标签拖回:子窗口里按住项目标签拖动,指针进入主窗口
标签条时**立即**合并(不松手),拖拽会话无缝转移给主窗口继续。

框架现状:指针按下后,隐式抓取属于发起窗口——后续 Move/Up(含出界坐标)
持续投递给发起窗口;另一个窗口收不到任何事件。要把拖拽会话"接棒"给同进程
的另一个窗口,唯一可行路径是:发起窗口把它收到的事件**翻译坐标后注入目标
窗口的指针管线**。这需要一个公开的合成指针事件入口。

## 需要的 API(公开,`include/huxerui/window.h`)

```cpp
class WindowHandle {
public:
  /// 向该窗口的指针管线投递一个合成指针事件:走正常的命中测试与手势识别
  /// 管线,等价于平台送来的事件。跨窗口拖拽接棒场景:先合成 Primary Down
  /// 开启接收方会话,之后镜像 Move,最后镜像 Up/Cancel 结束。
  /// 应用线程调用;position 为窗口本地逻辑坐标。
  void DispatchPointerEvent(const PointerEvent& event) const;
};
```

## 实现要点

1. `detail::WindowService`(`src/application/window_internal.h` + `window.cpp`)
   增加 `void DispatchPointer(const PointerEvent&) const`,转发到
   `ui_window_->HandlePointerEvent(event)` —— 该方法是 UiWindow 公有入口
   (`app.h`,平台适配器的 `SendPointer` 就是调它),共享管线,无需平台分叉。
2. `ui_window_ == nullptr`(窗口已退役)时 no-op,不抛异常。
3. 参考事件形状(Linux 适配层 `LinuxUiWindow::SendPointer`):
   `pointer_id = 0`、`device_kind = PointerDeviceKind::Mouse`、按下时
   `changed_button = pressed_buttons = PointerButton::Primary`、modifiers 默认。

## 语义要求(验收标准)

1. **Down 走完整命中测试**:注入的 Down 命中挂了 DragGesture 的视图后,后续
   注入的 Move 按 slop 阈值接受手势并驱动 Started/Changed——与真实按压完全一致。
2. **注入序列与真实事件流无缝衔接**:注入 Down 后,若真实指针抓取因源窗口
   退役落回本窗口(隐式抓取消失,指针下的窗口接管),后续真实 Move/Up 应继续
   驱动同一手势会话(pointer_id 与按键状态一致即可,不得因"来源不同"分裂会话)。
3. 注入 Up/Cancel 正常结束会话,不留悬挂的抓取态(不能吞掉后续真实点击)。

## 建议测试

加入 `[linux-native]`(或新增同形态用例):真实两窗口场景,A 窗口注入 Down 到
B 窗口某 DragSource 视图(经 `Position()` 换算坐标),注入 Move 若干步、Up 结束,
断言 B 窗口手势事件序列 = Started/Changed…/Ended;中途 retire 掉 A,会话不中断。
