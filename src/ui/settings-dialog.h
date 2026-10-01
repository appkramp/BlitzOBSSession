#pragma once

// Tools → Blitz Session Stats: the key, the realm, the period, the overlay
// language, and what the connection is doing right now.

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QRadioButton;

namespace bss {

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit SettingsDialog(QWidget *parent);

	void loadConfig();

private:
	void save();
	void updateStatus();
	void updateKeyHint();

	QLineEdit *key_;
	QLabel *keyWarning_;
	QComboBox *realm_;
	QRadioButton *today_;
	QRadioButton *since_;
	QDateTimeEdit *sinceEdit_;
	QComboBox *language_;
	QLineEdit *server_;
	QLabel *status_;
};

} // namespace bss
