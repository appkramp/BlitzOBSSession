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
class QTimer;

namespace bss {

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit SettingsDialog(QWidget *parent);

	void loadConfig();

private:
	// Every change applies at once — there is no Save: a key pasted is the key
	// used. Typing is waited out briefly, so a key is not tried half-typed.
	void apply();
	void applySoon();
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
	QTimer *applyTimer_;
	bool loading_ = false;
};

} // namespace bss
