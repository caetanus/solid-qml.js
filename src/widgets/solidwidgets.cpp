#include "solidwidgets.h"

#include "primitives.h"
#include "button.h"
#include "focusring.h"

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
}

} // namespace SolidWidgets
