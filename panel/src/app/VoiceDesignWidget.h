#pragma once
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>

class QComboBox;
class QFormLayout;
class QSpinBox;

namespace VoicePanel {
class VoiceDesignWidget : public QGroupBox {
    Q_OBJECT
public:
    explicit VoiceDesignWidget(QWidget *parent = nullptr);
    void setModels(const QJsonArray &models);
    void setModel(const QString &model);
    void setInstructions(const QString &instructions);
    QString instructions() const;
    QString savedInstructions() const { return saved_; }
    QJsonObject parameters() const { return parameters_; }
    void setOptions(const QJsonObject &options);
    QJsonObject options() const;
    QJsonObject savedOptions() const { return savedOptions_; }
signals:
    void changed();
    void speedSupported(bool supported);
private:
    void refresh();
    QString model_;
    QString saved_;
    QJsonArray models_;
    QJsonObject parameters_;
    QJsonObject savedOptions_;
    QSpinBox *steps_;
    QSpinBox *seed_;
    QFormLayout *form_;
    QMap<QString, QComboBox *> fields_;
};
}
