#pragma once

#include "CaptionPane.h"

#include <QAbstractNativeEventFilter>
#include <QWidget>

// Qt owns the space in the splitter. The caption renderer and its controls
// keep their original HWNDs, input handling, and UI Automation identities.
class CaptionHost final : public QWidget, private QAbstractNativeEventFilter {
public:
    explicit CaptionHost(QWidget* parent);
    ~CaptionHost() override;
    bool initialize(const Settings& settings);

protected:
    void focusInEvent(QFocusEvent* event) override;
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;

private:
    void fitPane();
    CaptionPane pane_;
};
