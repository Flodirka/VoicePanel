#include "app/VoiceDesignWidget.h"
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>

using namespace VoicePanel;

VoiceDesignWidget::VoiceDesignWidget(QWidget *parent) : QGroupBox(tr("Voice design"), parent),
    form_(new QFormLayout(this)) {
    setObjectName("voiceDesign");
    const QList<QPair<QString, QString>> labels = {
        {"gender", tr("Gender")}, {"age", tr("Age")}, {"pitch", tr("Pitch")},
        {"style", tr("Style")}, {"accent", tr("English accent")}, {"dialect", tr("Chinese dialect")},
    };
    for (const auto &label : labels) {
        auto *combo = new QComboBox;
        combo->setObjectName("voiceDesign_" + label.first);
        fields_.insert(label.first, combo);
        form_->addRow(label.second, combo);
        connect(combo, &QComboBox::currentIndexChanged, this, [this] {
            QStringList values;
            for (auto *field : fields_) if (!field->currentData().toString().isEmpty())
                values << field->currentData().toString();
            saved_ = values.join(", ");
            emit changed();
        });
    }
    steps_ = new QSpinBox;
    steps_->setObjectName("voiceDesignSteps");
    steps_->setRange(1, 128);
    steps_->setValue(32);
    steps_->setToolTip(tr("Fewer steps are faster. Compare the voice quality before reducing them."));
    seed_ = new QSpinBox;
    seed_->setObjectName("voiceDesignSeed");
    seed_->setRange(-1, 2147483647);
    seed_->setSpecialValueText(tr("Random"));
    seed_->setValue(-1);
    form_->addRow(tr("Generation steps"), steps_);
    form_->addRow(tr("Seed"), seed_);
    for (auto *spin : {steps_, seed_}) connect(spin, &QSpinBox::valueChanged, this, [this] {
        savedOptions_ = options();
        emit changed();
    });
    auto *hint = new QLabel(tr("Design creates a voice without a reference recording. "
                              "Use a recorded voice profile for cloning. Accent applies to English; dialect applies to Chinese."));
    hint->setWordWrap(true);
    hint->setProperty("role", "caption");
    form_->addRow(hint);
    hide();
}

void VoiceDesignWidget::setModels(const QJsonArray &models) { models_ = models; refresh(); }
void VoiceDesignWidget::setModel(const QString &model) { model_ = model; refresh(); }
void VoiceDesignWidget::setInstructions(const QString &instructions) { saved_ = instructions; refresh(); }
void VoiceDesignWidget::setOptions(const QJsonObject &options) { savedOptions_ = options; refresh(); }

QJsonObject VoiceDesignWidget::options() const {
    if (isHidden()) return {};
    QJsonObject result;
    const auto names = parameters_.value("options").toArray();
    if (names.contains("steps")) result.insert("steps", steps_->value());
    if (names.contains("num_inference_steps")) result.insert("num_inference_steps", steps_->value());
    if (names.contains("seed") && seed_->value() >= 0) result.insert("seed", seed_->value());
    return result;
}

QString VoiceDesignWidget::instructions() const {
    return isHidden() ? QString() : saved_;
}

void VoiceDesignWidget::refresh() {
    QJsonObject params;
    for (const auto &value : models_) {
        const auto entry = value.toObject();
        if (entry.value("id").toString() == model_) params = entry.value("speech_parameters").toObject();
    }
    const bool supported = params.value("instructions").toBool();
    parameters_ = params;
    const auto names = params.value("options").toArray();
    {
        const QSignalBlocker blockSteps(steps_), blockSeed(seed_);
        steps_->setValue(savedOptions_.value("steps").toInt(savedOptions_.value("num_inference_steps").toInt(32)));
        seed_->setValue(savedOptions_.value("seed").toInt(-1));
    }
    form_->setRowVisible(steps_, names.contains("steps") || names.contains("num_inference_steps"));
    form_->setRowVisible(seed_, names.contains("seed"));
    const auto schema = params.value("voice_design").toObject();
    const QStringList selected = saved_.split(", ", Qt::SkipEmptyParts);
    for (auto it = fields_.cbegin(); it != fields_.cend(); ++it) {
        const QSignalBlocker blocker(it.value());
        it.value()->clear();
        it.value()->addItem(tr("Automatic"), QString());
        const auto choices = schema.value(it.key()).toArray();
        for (const auto &choice : choices) it.value()->addItem(choice.toString(), choice.toString());
        for (const QString &value : selected) {
            const int index = it.value()->findData(value);
            if (index >= 0) it.value()->setCurrentIndex(index);
        }
        form_->setRowVisible(it.value(), !choices.isEmpty());
    }
    setVisible(supported);
    emit speedSupported(model_ != "profile" && params.value("speed").toBool(true));
}
