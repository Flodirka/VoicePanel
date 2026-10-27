#include "app/ServerSettingsDialog.h"

#include "app/UiKit.h"
#include "core/ServiceClient.h"
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

using namespace VoicePanel;

namespace {

QString deviceName(const QString &device) {
    if (device == "gpu") return QObject::tr("GPU (CUDA)");
    if (device == "cpu") return QObject::tr("CPU");
    return device;
}

} // namespace

ServerSettingsDialog::ServerSettingsDialog(const QString &serviceUrl, QWidget *parent)
    : QDialog(parent), client_(new ServiceClient(this)) {
    client_->setBaseUrl(serviceUrl);
    buildUi();
    load();
}

void ServerSettingsDialog::buildUi() {
    setWindowTitle(tr("Server settings"));
    setMinimumWidth(520);
    resize(560, 420);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 8, 16, 12);
    root->setSpacing(10);
    auto *hint = new QLabel(tr("Choose the processor for each inference process. "
                               "A choice is saved in the service settings and used by the "
                               "next request; a request in progress is never interrupted."));
    hint->setWordWrap(true);
    hint->setProperty("role", "caption");
    root->addWidget(hint);

    auto *box = new QGroupBox(tr("Processors"));
    rowsLayout_ = new QVBoxLayout(box);
    rowsLayout_->setSpacing(10);
    root->addWidget(box);

    auto *shared = new QLabel(tr("Recognition and the built-in voices share one process, "
                                 "so they always use the same processor."));
    shared->setWordWrap(true);
    shared->setProperty("role", "caption");
    root->addWidget(shared);

    status_ = new QLabel(tr("Reading the service…"));
    status_->setObjectName("serverComputeStatus");
    status_->setWordWrap(true);
    status_->setProperty("role", "caption");
    root->addWidget(status_);
    root->addStretch(1);

    auto *buttons = new QHBoxLayout;
    auto *close = new QPushButton(tr("Close"));
    close->setProperty("variant", "ghost");
    saveButton_ = new QPushButton(tr("Save"));
    saveButton_->setObjectName("saveServerCompute");
    saveButton_->setProperty("variant", "primary");
    saveButton_->setEnabled(false);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(saveButton_, &QPushButton::clicked, this, &ServerSettingsDialog::save);
    buttons->addStretch(1);
    buttons->addWidget(saveButton_);
    buttons->addWidget(close);
    root->addLayout(buttons);
    UiKit::wrapDialog(*this, windowTitle());
}

void ServerSettingsDialog::load() {
    QPointer<ServerSettingsDialog> self(this);
    client_->getJson("/admin/compute", [this, self](bool ok, QByteArray body, QString error) {
        if (!self) return;
        if (!ok) {
            status_->setText(tr("Cannot read the service settings: %1").arg(errorDetail(body, error)));
            saveButton_->setEnabled(false);
            return;
        }
        showSnapshot(body);
    });
}

void ServerSettingsDialog::showSnapshot(const QByteArray &body) {
    const auto groups = QJsonDocument::fromJson(body).object().value("groups").toArray();
    loading_ = true;
    serverDevices_.clear();
    for (const auto &entry : groups) {
        const auto group = entry.toObject();
        const QString id = group.value("id").toString();
        if (id.isEmpty()) continue;
        serverDevices_[id] = group.value("device").toString();
        if (!rows_.contains(id)) {
            auto *row = new QWidget;
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(10);
            auto *name = new QLabel(group.value("label").toString());
            name->setWordWrap(true);
            layout->addWidget(name, 1);
            auto *device = new QComboBox;
            device->setObjectName("device-" + id);
            device->setMinimumWidth(140);
            layout->addWidget(device);
            auto *state = new QLabel;
            state->setProperty("role", "caption");
            state->setMinimumWidth(96);
            layout->addWidget(state);
            rowsLayout_->addWidget(row);
            connect(device, &QComboBox::currentIndexChanged, this,
                    &ServerSettingsDialog::updateSaveState);
            rows_[id] = {device, state};
        }
        Row row = rows_[id];
        const QSignalBlocker blocker(row.device);
        row.device->clear();
        for (const auto &value : group.value("available").toArray())
            row.device->addItem(deviceName(value.toString()), value.toString());
        const QString device = group.value("device").toString();
        int index = row.device->findData(device);
        if (index < 0 && !device.isEmpty()) {
            row.device->addItem(tr("%1 (unavailable)").arg(deviceName(device)), device);
            index = row.device->findData(device);
        }
        if (index >= 0) row.device->setCurrentIndex(index);
        row.state->setText(group.value("busy").toBool() ? tr("Busy")
                          : group.value("running").toBool() ? tr("Running") : tr("Stopped"));
    }
    loading_ = false;
    status_->setText(tr("Current choices are shown. Press Save to apply a change."));
    updateSaveState();
}

void ServerSettingsDialog::restoreChoices() {
    loading_ = true;
    for (auto it = rows_.cbegin(); it != rows_.cend(); ++it) {
        const QSignalBlocker blocker(it.value().device);
        const int index = it.value().device->findData(serverDevices_.value(it.key()));
        if (index >= 0) it.value().device->setCurrentIndex(index);
    }
    loading_ = false;
    updateSaveState();
}

void ServerSettingsDialog::updateSaveState() {
    if (loading_) return;
    bool changed = false;
    for (auto it = rows_.cbegin(); it != rows_.cend(); ++it)
        if (it.value().device->currentData().toString() != serverDevices_.value(it.key())) changed = true;
    saveButton_->setEnabled(changed);
}

void ServerSettingsDialog::save() {
    QJsonObject devices;
    for (auto it = rows_.cbegin(); it != rows_.cend(); ++it)
        devices.insert(it.key(), it.value().device->currentData().toString());
    saveButton_->setEnabled(false);
    status_->setText(tr("Applying…"));
    QPointer<ServerSettingsDialog> self(this);
    const QByteArray payload = QJsonDocument(QJsonObject{{"devices", devices}}).toJson();
    client_->postJson("/admin/compute", payload, [this, self](bool ok, QByteArray body, QString error) {
        if (!self) return;
        if (!ok) {
            status_->setText(tr("Not applied: %1").arg(errorDetail(body, error)));
            restoreChoices();   // the service kept its choices; show them again
            return;
        }
        showSnapshot(body);
        status_->setText(tr("Saved. The new processor is used by the next request."));
    });
}

QString ServerSettingsDialog::errorDetail(const QByteArray &body, const QString &fallback) const {
    const auto detail = QJsonDocument::fromJson(body).object().value("detail").toString();
    return detail.isEmpty() ? fallback : detail;
}
