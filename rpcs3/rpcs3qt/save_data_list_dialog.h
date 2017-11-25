#pragma once

// I just want the struct for the save data.
#include "stdafx.h"
#include "Emu/System.h"
#include "Emu/Memory/Memory.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "gui_settings.h"

#include <QTableWidget>
#include <QDialog>
#include <QLabel>

//Display a list of SaveData. Would need to be initialized.
//Can also be used as a Save Data Chooser.
class save_data_list_dialog : public QDialog
{
	Q_OBJECT

	enum selection_code
	{
		new_save = -1,
		canceled = -2
	};

public:
	explicit save_data_list_dialog(const std::vector<SaveDataEntry>& entries, s32 focusedEntry, u32 op, vm::ptr<CellSaveDataListSet>, QWidget* parent = nullptr);

	s32 GetSelection();
private Q_SLOTS:
	void OnEntryInfo();
	void OnSort(int logicalIndex);
private:
	void UpdateSelectionLabel(void);
	void UpdateList(void);

	s32 m_entry;
	QLabel* m_entry_label;

	QTableWidget* m_list;
	std::vector<SaveDataEntry> m_save_entries;

	std::shared_ptr<gui_settings> m_gui_settings;

	int m_sort_column;
	bool m_sort_ascending;
};
