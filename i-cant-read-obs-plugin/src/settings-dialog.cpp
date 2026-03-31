#include "settings-dialog.hpp"
#include <obs-frontend-api.h>

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QLabel>

static QDoubleSpinBox *make_double(double min, double max, double step, double val)
{
	auto *w = new QDoubleSpinBox;
	w->setRange(min, max);
	w->setSingleStep(step);
	w->setValue(val);
	return w;
}

static QSpinBox *make_int(int min, int max, int step, int val)
{
	auto *w = new QSpinBox;
	w->setRange(min, max);
	w->setSingleStep(step);
	w->setValue(val);
	return w;
}

static QLabel *hint(const char *text)
{
	auto *l = new QLabel(text);
	l->setWordWrap(true);
	l->setStyleSheet("color: gray; font-size: 11px;");
	return l;
}

SettingsDialog::SettingsDialog(obs_data_t *s, QWidget *parent) : QDialog(parent), settings(s)
{
	setWindowTitle("I Can't Read Settings");
	setMinimumWidth(420);

	auto *root = new QVBoxLayout(this);

	// --- Frame Stability ---
	auto *stab_group = new QGroupBox("Frame Stability");
	auto *stab_form = new QFormLayout(stab_group);

	stability_frames = make_int(1, 60, 1, obs_data_get_int(s, "stability_frames"));
	stab_form->addRow("Stability Frames", stability_frames);
	stab_form->addRow("", hint("Consecutive stable frames before OCR triggers.\n"
				   "Fast dialogue missed → decrease  |  Mid-animation trigger → increase"));

	stability_threshold = make_double(0.0, 255.0, 0.1, obs_data_get_double(s, "stability_threshold"));
	stab_form->addRow("Stability Threshold", stability_threshold);
	stab_form->addRow("", hint("Max mean absolute diff (0–255) to count as stable.\n"
				   "Mid-animation trigger → decrease  |  Animation done but missed → increase"));

	root->addWidget(stab_group);

	// --- Change Detection ---
	auto *chng_group = new QGroupBox("Change Detection");
	auto *chng_form = new QFormLayout(chng_group);

	text_lap_threshold = make_double(0.0, 200.0, 0.5, obs_data_get_double(s, "text_lap_threshold"));
	chng_form->addRow("Text Lap Threshold", text_lap_threshold);
	chng_form->addRow("", hint("Laplacian variance below this = no text present."));

	change_threshold = make_double(0.0, 255.0, 0.1, obs_data_get_double(s, "change_threshold"));
	chng_form->addRow("Change diff Threshold", change_threshold);
	chng_form->addRow("", hint("Min mean absolute diff (0-255) from previous OCR frame to count as changed.\n"
				   "Missed dialogue change → decrease  |  False change trigger → increase"));

	root->addWidget(chng_group);

	// --- Luma / Background ---
	auto *luma_group = new QGroupBox("Luma / Background");
	auto *luma_form = new QFormLayout(luma_group);

	black_threshold = make_int(0, 255, 1, obs_data_get_int(s, "black_threshold"));
	luma_form->addRow("Black Threshold", black_threshold);
	luma_form->addRow("", hint("Pixel luma ≤ this = background, excluded from diff/hash.\n"
				   "0 = disabled. Use ~10 with OBS Luma Key."));

	luma_noise_floor = make_double(0.0, 100.0, 0.5, obs_data_get_double(s, "luma_noise_floor"));
	luma_form->addRow("Luma Noise Floor", luma_noise_floor);
	luma_form->addRow("", hint("Per-pixel diff below this is zeroed. Suppresses compression noise.\n"
				   "0 = disabled."));

	root->addWidget(luma_group);

	// --- Laplacian Carve ---
	auto *lap_group = new QGroupBox("Laplacian Carve");
	lap_group->setCheckable(true);
	auto *lap_form = new QFormLayout(lap_group);

	use_lap_carve = new QCheckBox;
	use_lap_carve->setChecked(obs_data_get_bool(s, "use_lap_carve"));
	// drive the groupbox checkable state from the checkbox
	lap_group->setChecked(use_lap_carve->isChecked());
	connect(lap_group, &QGroupBox::toggled, use_lap_carve, &QCheckBox::setChecked);

	lap_carve_edge_threshold = make_double(0.0, 100.0, 0.5, obs_data_get_double(s, "lap_carve_edge_threshold"));
	lap_form->addRow("Edge Threshold", lap_carve_edge_threshold);

	lap_dilate_kernel = make_int(1, 31, 2, obs_data_get_int(s, "lap_dilate_kernel"));
	lap_form->addRow("Dilate Kernel", lap_dilate_kernel);
	lap_form->addRow("", hint("Must be odd. Covers ~kernel/2 px around each edge."));

	root->addWidget(lap_group);

	// --- Buttons ---
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		save();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

void SettingsDialog::save()
{
	obs_data_set_int(settings, "stability_frames", stability_frames->value());
	obs_data_set_double(settings, "stability_threshold", stability_threshold->value());
	obs_data_set_double(settings, "text_lap_threshold", text_lap_threshold->value());
	obs_data_set_double(settings, "change_threshold", change_threshold->value());
	obs_data_set_int(settings, "black_threshold", black_threshold->value());
	obs_data_set_double(settings, "luma_noise_floor", luma_noise_floor->value());
	obs_data_set_bool(settings, "use_lap_carve", use_lap_carve->isChecked());
	obs_data_set_double(settings, "lap_carve_edge_threshold", lap_carve_edge_threshold->value());
	obs_data_set_int(settings, "lap_dilate_kernel", lap_dilate_kernel->value());
}

extern "C" void show_settings_dialog(obs_data_t *settings)
{
	auto *dialog = new SettingsDialog(settings, static_cast<QWidget *>(obs_frontend_get_main_window()));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->exec();
}
