// Copyright 2013 Velodyne Acoustics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "vvCalibrationDialog.h"

#include "ui_vvCalibrationDialog.h"

#include <pqApplicationCore.h>
#include <pqSettings.h>

#include <QDialog>
#include <QFileDialog>

//-----------------------------------------------------------------------------
class vvCalibrationDialog::pqInternal : public Ui::vvCalibrationDialog
{
public:
  pqInternal() : Settings(pqApplicationCore::instance()->settings()) {}

  void saveFileList();
  void saveSelectedRow();
  void restoreSelectedRow();

  void saveSensorTransform();
  void restoreSensorTransform();

  pqSettings* const Settings;
};

//-----------------------------------------------------------------------------
void vvCalibrationDialog::pqInternal::saveFileList()
{
  QStringList files;
  for (int i = 1; i < this->ListWidget->count(); ++i)
    {
    files << this->ListWidget->item(i)->text();
    }

  this->Settings->setValue(
    "VelodyneHDLPlugin/CalibrationFileDialog/Files", files);
}

//-----------------------------------------------------------------------------
void vvCalibrationDialog::pqInternal::saveSelectedRow()
{
  this->Settings->setValue(
    "VelodyneHDLPlugin/CalibrationFileDialog/CurrentRow",
    this->ListWidget->currentRow());
}

//-----------------------------------------------------------------------------
void vvCalibrationDialog::pqInternal::restoreSelectedRow()
{
  int row = this->Settings->value(
              "VelodyneHDLPlugin/CalibrationFileDialog/CurrentRow").toInt();
  this->ListWidget->setCurrentRow(row);
}

//-----------------------------------------------------------------------------
void vvCalibrationDialog::pqInternal::saveSensorTransform()
{
  this->Settings->setValue(
    "VelodyneHDLPlugin/CalibrationFileDialog/SensorOriginX",
    this->OriginXSpinBox->value());
  this->Settings->setValue(
    "VelodyneHDLPlugin/CalibrationFileDialog/SensorOriginY",
    this->OriginYSpinBox->value());
  this->Settings->setValue(
    "VelodyneHDLPlugin/CalibrationFileDialog/SensorOriginZ",
    this->OriginZSpinBox->value());
  this->Settings->setValue(
    "VelodyneHDLPlugin/CalibrationFileDialog/SensorYaw",
    this->YawSpinBox->value());
  this->Settings->setValue(
    "VelodyneHDLPlugin/CalibrationFileDialog/SensorPitch",
    this->PitchSpinBox->value());
  this->Settings->setValue(
    "VelodyneHDLPlugin/CalibrationFileDialog/SensorRoll",
    this->RollSpinBox->value());
}

//-----------------------------------------------------------------------------
void vvCalibrationDialog::pqInternal::restoreSensorTransform()
{
  this->OriginXSpinBox->setValue(
    this->Settings->value(
      "VelodyneHDLPlugin/CalibrationFileDialog/SensorOriginX",
      this->OriginXSpinBox->value()).toDouble());
  this->OriginYSpinBox->setValue(
    this->Settings->value(
      "VelodyneHDLPlugin/CalibrationFileDialog/SensorOriginY",
      this->OriginYSpinBox->value()).toDouble());
  this->OriginZSpinBox->setValue(
    this->Settings->value(
      "VelodyneHDLPlugin/CalibrationFileDialog/SensorOriginZ",
      this->OriginZSpinBox->value()).toDouble());
  this->YawSpinBox->setValue(
    this->Settings->value(
      "VelodyneHDLPlugin/CalibrationFileDialog/SensorYaw",
      this->YawSpinBox->value()).toDouble());
  this->PitchSpinBox->setValue(
    this->Settings->value(
      "VelodyneHDLPlugin/CalibrationFileDialog/SensorPitch",
      this->PitchSpinBox->value()).toDouble());
  this->RollSpinBox->setValue(
    this->Settings->value(
      "VelodyneHDLPlugin/CalibrationFileDialog/SensorRoll",
      this->RollSpinBox->value()).toDouble());
}

//-----------------------------------------------------------------------------
vvCalibrationDialog::vvCalibrationDialog(QWidget *p)
  : QDialog(p), Internal(new pqInternal)
{
  this->Internal->setupUi(this);

  this->Internal->ListWidget->addItem("(None)");
  this->Internal->ListWidget->addItems(this->calibrationFiles());

  connect(this->Internal->ListWidget, SIGNAL(currentRowChanged(int)),
          this, SLOT(onCurrentRowChanged(int)));
  connect(this->Internal->AddButton, SIGNAL(clicked()),
          this, SLOT(addFile()));
  connect(this->Internal->RemoveButton, SIGNAL(clicked()),
          this, SLOT(removeSelectedFile()));

  this->Internal->restoreSelectedRow();
  this->Internal->restoreSensorTransform();

  const QVariant& geometry =
    this->Internal->Settings->value(
      "VelodyneHDLPlugin/CalibrationFileDialog/Geometry");
  this->restoreGeometry(geometry.toByteArray());
}

//-----------------------------------------------------------------------------
vvCalibrationDialog::~vvCalibrationDialog()
{
  this->Internal->Settings->setValue(
    "VelodyneHDLPlugin/CalibrationFileDialog/Geometry", this->saveGeometry());
}

//-----------------------------------------------------------------------------
QStringList vvCalibrationDialog::calibrationFiles() const
{
  return this->Internal->Settings->value(
           "VelodyneHDLPlugin/CalibrationFileDialog/Files").toStringList();
}

//-----------------------------------------------------------------------------
QString vvCalibrationDialog::selectedCalibrationFile() const
{
  const int row = this->Internal->ListWidget->currentRow();
  if (row > 0)
    {
    return this->Internal->ListWidget->item(row)->text();
    }
  return QString();
}

//-----------------------------------------------------------------------------
QMatrix4x4 vvCalibrationDialog::sensorTransform() const
{
  QMatrix4x4 transform;
  transform.rotate(this->Internal->YawSpinBox->value(), 0.0, 0.0, 1.0);
  transform.rotate(this->Internal->PitchSpinBox->value(), 1.0, 0.0, 0.0);
  transform.rotate(this->Internal->RollSpinBox->value(), 0.0, 1.0, 0.0);
  transform.translate(this->Internal->OriginXSpinBox->value(),
                      this->Internal->OriginYSpinBox->value(),
                      this->Internal->OriginZSpinBox->value());

  return transform;
}

//-----------------------------------------------------------------------------
void vvCalibrationDialog::accept()
{
  this->Internal->saveSelectedRow();
  this->Internal->saveSensorTransform();
  QDialog::accept();
}

//-----------------------------------------------------------------------------
void vvCalibrationDialog::onCurrentRowChanged(int row)
{
  this->Internal->RemoveButton->setEnabled(row != 0);
}

//-----------------------------------------------------------------------------
void vvCalibrationDialog::addFile()
{
  QString defaultDir =
    this->Internal->Settings->value("VelodyneHDLPlugin/OpenData/DefaultDir",
                                    QDir::homePath()).toString();


  QString selectedFiler("*.xml");
  QString fileName = QFileDialog::getOpenFileName(
                       this, tr("Choose Calibration File"), defaultDir,
                       tr("xml (*.xml)"), &selectedFiler);

  if (fileName.isEmpty())
    {
    return;
    }

  this->Internal->ListWidget->addItem(fileName);
  this->Internal->ListWidget->setCurrentRow(
    this->Internal->ListWidget->count() - 1);
  this->Internal->saveFileList();

  this->Internal->Settings->setValue(
    "VelodyneHDLPlugin/OpenData/DefaultDir",
    QFileInfo(fileName).absoluteDir().absolutePath());
}

//-----------------------------------------------------------------------------
void vvCalibrationDialog::removeSelectedFile()
{
  const int row = this->Internal->ListWidget->currentRow();
  if (row > 0)
    {
    delete this->Internal->ListWidget->takeItem(row);
    this->Internal->saveFileList();
    }
}
