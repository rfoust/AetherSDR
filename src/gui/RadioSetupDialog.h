#pragma once

#include <QDialog>

class QTabWidget;
class QLabel;
class QLineEdit;

namespace AetherSDR {

class RadioModel;
class AudioEngine;

// Radio Setup dialog — tabbed configuration window matching SmartSDR's
// Settings → Radio Setup. Shows radio info, GPS, TX, RX, filters, etc.
class RadioSetupDialog : public QDialog {
    Q_OBJECT

public:
    explicit RadioSetupDialog(RadioModel* model, AudioEngine* audio = nullptr,
                              QWidget* parent = nullptr);

private:
    QWidget* buildRadioTab();
    QWidget* buildNetworkTab();
    QWidget* buildGpsTab();
    QWidget* buildTxTab();
    QWidget* buildPhoneCwTab();
    QWidget* buildRxTab();
    QWidget* buildAudioTab();
    QWidget* buildFiltersTab();
    QWidget* buildXvtrTab();

    RadioModel*  m_model;
    AudioEngine* m_audio{nullptr};

    // Radio tab fields
    QLabel* m_serialLabel{nullptr};
    QLabel* m_hwVersionLabel{nullptr};
    QLabel* m_regionLabel{nullptr};
    QLabel* m_optionsLabel{nullptr};
    QLabel* m_remoteOnLabel{nullptr};
    QLabel* m_modelLabel{nullptr};
    QLineEdit* m_nicknameEdit{nullptr};
    QLineEdit* m_callsignEdit{nullptr};
    QPushButton* m_remoteOnBtn{nullptr};
};

} // namespace AetherSDR
