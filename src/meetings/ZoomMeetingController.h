#pragma once

#include <QJsonObject>
#include <QString>

#include <vector>
#include <optional>
#include <functional>

#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

// Reads and changes the microphone and computer-audio connection of a Zoom tab.
// Every change reads the control first, acts only when the state differs, and
// reads it again afterwards: a result is reported as changed only once Zoom has
// confirmed the new state.
class ZoomMeetingController
{
public:
    explicit ZoomMeetingController(IUIAutomation* automation,
        std::function<void(const QString&)> diagnostic = {});

    QJsonObject activateTab(quintptr hwndValue,
                            const QString& tabRuntimeId,
                            const QString& tabName,
                            bool bringToFront) const;

    QJsonObject readMicrophone(quintptr hwndValue,
                               const QString& tabRuntimeId,
                               const QString& tabName) const;

    // Selects the tab, brings its window forward, then reads the microphone.
    QJsonObject activateAndReadMicrophone(quintptr hwndValue,
                                          const QString& tabRuntimeId,
                                          const QString& tabName) const;

    QJsonObject setMicrophoneMuted(quintptr hwndValue,
                                   const QString& tabRuntimeId,
                                   const QString& tabName,
                                   bool muted) const;

    // Joins or leaves the meeting's computer audio. Leaving silences the
    // meeting in both directions until it is joined again.
    QJsonObject setAudioConnected(quintptr hwndValue,
                                  const QString& tabRuntimeId,
                                  const QString& tabName,
                                  bool connected) const;

    QJsonObject readAudioConnection(quintptr hwndValue,
                                    const QString& tabRuntimeId,
                                    const QString& tabName) const;

private:
    struct Control
    {
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        QString name;
        CONTROLTYPEID type = 0;
        bool selected = false;
        bool computerAudioPanel = false;
    };

    // For a window-wide search, restrict detached chooser/menu controls to
    // the meeting identified by popupDocument.
    std::vector<Control> collectControls(IUIAutomationElement* root, HWND hwnd,
                                         IUIAutomationElement* popupDocument = nullptr) const;
    QJsonObject controlAudioConnection(quintptr hwndValue,
                                       const QString& tabRuntimeId,
                                       const QString& tabName,
                                       std::optional<bool> connected) const;
    Microsoft::WRL::ComPtr<IUIAutomationElement> rootOf(HWND hwnd) const;

    Microsoft::WRL::ComPtr<IUIAutomationElement> findTab(
        const Microsoft::WRL::ComPtr<IUIAutomationElement>& root,
        const QString& tabRuntimeId,
        const QString& tabName) const;

    Microsoft::WRL::ComPtr<IUIAutomationElement> findLiveDocument(HWND hwnd,
        const QString& selectedTabName = {}) const;

    Microsoft::WRL::ComPtr<IUIAutomationElement> findMicrophoneButton(
        IUIAutomationElement* document) const;

    QJsonObject readMicrophoneInWindow(HWND hwnd) const;

    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
    std::function<void(const QString&)> diagnostic_;
};
