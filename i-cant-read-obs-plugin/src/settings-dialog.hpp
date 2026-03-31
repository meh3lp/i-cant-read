#pragma once

#ifdef __cplusplus
#include <obs/obs-module.h>
#include <QDialog>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>

class SettingsDialog : public QDialog {
	Q_OBJECT
public:
	explicit SettingsDialog(obs_data_t *settings, QWidget *parent = nullptr);
	void save();

private:
	obs_data_t *settings;
	QSpinBox *stability_frames;
	QDoubleSpinBox *stability_threshold;
	QDoubleSpinBox *text_lap_threshold;
	QDoubleSpinBox *change_threshold;
	QSpinBox *black_threshold;
	QDoubleSpinBox *luma_noise_floor;
	QCheckBox *use_lap_carve;
	QDoubleSpinBox *lap_carve_edge_threshold;
	QSpinBox *lap_dilate_kernel;
};
#endif

// C-visible bridge — callable from plugin-main.c
#ifdef __cplusplus
extern "C" {
#endif

void show_settings_dialog(obs_data_t *settings);

#ifdef __cplusplus
}
#endif
