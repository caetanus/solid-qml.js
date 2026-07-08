#include "solidwidgets.h"

#include "primitives.h"
#include "button.h"
#include "focusring.h"
#include "indicators.h"
#include "sliders.h"
#include "delaybutton.h"
#include "spindial.h"
#include "tabbar.h"
#include "textinputs.h"
#include "toolbar.h"
#include "tumbler.h"
#include "toggles.h"

namespace SolidWidgets {

void registerTypes()
{
    // Same module URI as the remaining qmldir composite types — the registrations merge into
    // one namespace, so the port can proceed widget by widget.
    qmlRegisterType<Div>("solidqml.Widgets", 1, 0, "Div");
    qmlRegisterType<Text>("solidqml.Widgets", 1, 0, "Text");
    qmlRegisterType<Image>("solidqml.Widgets", 1, 0, "Image");
    qmlRegisterType<Tabstop>("solidqml.Widgets", 1, 0, "Tabstop");
    qmlRegisterType<Button>("solidqml.Widgets", 1, 0, "Button");
    qmlRegisterType<RoundButton>("solidqml.Widgets", 1, 0, "RoundButton");
    qmlRegisterType<Fieldset>("solidqml.Widgets", 1, 0, "Fieldset");
    qmlRegisterType<StackView>("solidqml.Widgets", 1, 0, "StackView");
    qmlRegisterType<Progress>("solidqml.Widgets", 1, 0, "Progress");
    qmlRegisterType<BusyIndicator>("solidqml.Widgets", 1, 0, "BusyIndicator");
    qmlRegisterType<Checkbox>("solidqml.Widgets", 1, 0, "Checkbox");
    qmlRegisterType<Toggle>("solidqml.Widgets", 1, 0, "Toggle");
    qmlRegisterType<Slider>("solidqml.Widgets", 1, 0, "Slider");
    qmlRegisterType<RangeSlider>("solidqml.Widgets", 1, 0, "RangeSlider");
    qmlRegisterType<TextField>("solidqml.Widgets", 1, 0, "TextField");
    qmlRegisterType<TextArea>("solidqml.Widgets", 1, 0, "TextArea");
    qmlRegisterType<SpinBox>("solidqml.Widgets", 1, 0, "SpinBox");
    qmlRegisterType<Dial>("solidqml.Widgets", 1, 0, "Dial");
    qmlRegisterType<DelayButton>("solidqml.Widgets", 1, 0, "DelayButton");
    qmlRegisterType<Tumbler>("solidqml.Widgets", 1, 0, "Tumbler");
    qmlRegisterType<ToolButton>("solidqml.Widgets", 1, 0, "ToolButton");
    qmlRegisterType<ToolBar>("solidqml.Widgets", 1, 0, "ToolBar");
    qmlRegisterType<ToolSeparator>("solidqml.Widgets", 1, 0, "ToolSeparator");
    qmlRegisterType<TabBar>("solidqml.Widgets", 1, 0, "TabBar");
}

} // namespace SolidWidgets
