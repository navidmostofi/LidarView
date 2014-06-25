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
/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkVelodyneHDLReader.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkVelodyneHDLReader.h"

#include "vtkNew.h"
#include "vtkSmartPointer.h"
#include "vtkCellData.h"
#include "vtkCellArray.h"
#include "vtkUnsignedCharArray.h"
#include "vtkPoints.h"
#include "vtkDoubleArray.h"
#include "vtkShortArray.h"
#include "vtkUnsignedShortArray.h"
#include "vtkUnsignedIntArray.h"
#include "vtkDataArray.h"
#include "vtkFloatArray.h"

#include "vtkPolyData.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkMath.h"
#include "vtkStreamingDemandDrivenPipeline.h"

#include "vtkPacketFileReader.h"
#include "vtkPacketFileWriter.h"

#include "vtkWrappedTupleInterpolator.h"

#include <sstream>
#include <algorithm>
#include <cmath>


#include <boost/foreach.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>


#ifdef _MSC_VER
# include <boost/cstdint.hpp>
typedef boost::uint8_t uint8_t;
#else
# include <stdint.h>
#endif

namespace
{

#define HDL_Grabber_toRadians(x) ((x) * vtkMath::Pi() / 180.0)

const int HDL_NUM_ROT_ANGLES = 36001;
const int HDL_LASER_PER_FIRING = 32;
const int HDL_MAX_NUM_LASERS = 64;
const int HDL_FIRING_PER_PKT = 12;

enum HDLBlock
{
  BLOCK_0_TO_31 = 0xeeff,
  BLOCK_32_TO_63 = 0xddff
};

#pragma pack(push, 1)
typedef struct HDLLaserReturn
{
  unsigned short distance;
  unsigned char intensity;
} HDLLaserReturn;

struct HDLFiringData
{
  unsigned short blockIdentifier;
  unsigned short rotationalPosition;
  HDLLaserReturn laserReturns[HDL_LASER_PER_FIRING];
};

struct HDLDataPacket
{
  HDLFiringData firingData[HDL_FIRING_PER_PKT];
  unsigned int gpsTimestamp;
  unsigned char blank1;
  unsigned char blank2;
};

struct HDLLaserCorrection
{
  double azimuthCorrection;
  double verticalCorrection;
  double distanceCorrection;
  double verticalOffsetCorrection;
  double horizontalOffsetCorrection;
  double sinVertCorrection;
  double cosVertCorrection;
  double sinVertOffsetCorrection;
  double cosVertOffsetCorrection;
};

struct HDLRGB
{
  uint8_t r;
  uint8_t g;
  uint8_t b;
};
#pragma pack(pop)

}

//-----------------------------------------------------------------------------
class vtkVelodyneHDLReader::vtkInternal
{
public:

  vtkInternal()
  {
    this->Skip = 0;
    this->LastAzimuth = 0;
    this->Reader = 0;
    this->SplitCounter = 0;
    this->NumberOfTrailingFrames = 0;
    this->ApplyTransform = 0;
    this->PointsSkip = 0;

    for (size_t n = 0; n < HDL_MAX_NUM_LASERS; ++n)
      {
      this->LastPointId[n] = -1;
      }

    this->LaserSelection.resize(64, true);

    cos_lookup_table_ = NULL;
    sin_lookup_table_ = NULL;

    this->Init();
  }

  ~vtkInternal()
  {
    delete[] cos_lookup_table_;
    delete[] sin_lookup_table_;
  }

  std::vector<vtkSmartPointer<vtkPolyData> > Datasets;
  vtkSmartPointer<vtkPolyData> CurrentDataset;

  vtkSmartPointer<vtkWrappedTupleInterpolator> Interp;

  vtkPoints* Points;
  vtkUnsignedCharArray* Intensity;
  vtkUnsignedCharArray* LaserId;
  vtkUnsignedShortArray* Azimuth;
  vtkDoubleArray*        Distance;
  vtkUnsignedIntArray* Timestamp;
  vtkUnsignedIntArray* Flags;
  vtkShortArray* IntensityDelta;
  vtkDoubleArray* DistanceDelta;

  unsigned int LastAzimuth;
  vtkIdType LastPointId[HDL_MAX_NUM_LASERS];
  vtkIdType FirstPointIdThisReturn;

  std::vector<fpos_t> FilePositions;
  std::vector<int> Skips;
  int Skip;
  vtkPacketFileReader* Reader;

  int SplitCounter;
  int NumberOfTrailingFrames;
  int ApplyTransform;

  int PointsSkip;

  std::vector<bool> LaserSelection;

  double *cos_lookup_table_;
  double *sin_lookup_table_;
  HDLLaserCorrection laser_corrections_[HDL_MAX_NUM_LASERS];

  void SplitFrame(bool force=false);
  vtkSmartPointer<vtkPolyData> CreateData(vtkIdType numberOfPoints);
  vtkSmartPointer<vtkCellArray> NewVertexCells(vtkIdType numberOfVerts);

  void LoadHDL32Corrections();
  void LoadCorrectionsFile(const std::string& filename);
  void SetCorrectionsCommon();
  void Init();
  void InitTables();

  void ProcessHDLPacket(unsigned char *data, std::size_t bytesReceived);

  void ComputeOrientation(unsigned int timestamp,
                          unsigned int& azimuthOffset,
                          double translation[3]);

  // Process the laser return from the firing data
  // firingData - one of HDL_FIRING_PER_PKT from the packet
  // j - which laser
  // offset - either 0 or 32 to support 64-laser systems
  void ProcessFiring(HDLFiringData* firingData,
                     int offset,
                     unsigned int gpsTime,
                     unsigned int azimuthOffset,
                     const double translation[3]);

  void PushFiringData(const unsigned char laserId,
                      unsigned short azimuth,
                      const unsigned int timestamp,
                      const HDLLaserReturn* laserReturn,
                      const HDLLaserCorrection* correction,
                      const unsigned short azimuthAdjustment,
                      const double translation[3],
                      bool dualReturn);
};

//-----------------------------------------------------------------------------
vtkStandardNewMacro(vtkVelodyneHDLReader);

//-----------------------------------------------------------------------------
vtkVelodyneHDLReader::vtkVelodyneHDLReader()
{
  this->Internal = new vtkInternal;
  this->UnloadData();
  this->SetNumberOfInputPorts(0);
  this->SetNumberOfOutputPorts(1);
}

//-----------------------------------------------------------------------------
vtkVelodyneHDLReader::~vtkVelodyneHDLReader()
{
  delete this->Internal;
}

//-----------------------------------------------------------------------------
const std::string& vtkVelodyneHDLReader::GetFileName()
{
  return this->FileName;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetApplyTransform(int apply)
{
  if(apply != this->Internal->ApplyTransform)
    {
    this->Modified();
    }
  this->Internal->ApplyTransform = apply;
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::GetApplyTransform()
{
  return this->Internal->ApplyTransform;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetInterp(vtkWrappedTupleInterpolator* interp)
{
  this->Internal->Interp = interp;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetFileName(const std::string& filename)
{
  if (filename == this->FileName)
    {
    return;
    }

  this->FileName = filename;
  this->Internal->FilePositions.clear();
  this->Internal->Skips.clear();
  this->UnloadData();
  this->Modified();
}

//-----------------------------------------------------------------------------
const std::string& vtkVelodyneHDLReader::GetCorrectionsFile()
{
  return this->CorrectionsFile;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetLaserSelection(int x00, int x01, int x02, int x03, int x04, int x05, int x06, int x07,
                                        int x08, int x09, int x10, int x11, int x12, int x13, int x14, int x15,
                                        int x16, int x17, int x18, int x19, int x20, int x21, int x22, int x23,
                                        int x24, int x25, int x26, int x27, int x28, int x29, int x30, int x31,
                                        int x32, int x33, int x34, int x35, int x36, int x37, int x38, int x39,
                                        int x40, int x41, int x42, int x43, int x44, int x45, int x46, int x47,
                                        int x48, int x49, int x50, int x51, int x52, int x53, int x54, int x55,
                                        int x56, int x57, int x58, int x59, int x60, int x61, int x62, int x63)
{
  int mask[64] = {x00, x01, x02, x03, x04, x05, x06, x07,
                  x08, x09, x10, x11, x12, x13, x14, x15,
                  x16, x17, x18, x19, x20, x21, x22, x23,
                  x24, x25, x26, x27, x28, x29, x30, x31,
                  x32, x33, x34, x35, x36, x37, x38, x39,
                  x40, x41, x42, x43, x44, x45, x46, x47,
                  x48, x49, x50, x51, x52, x53, x54, x55,
                  x56, x57, x58, x59, x60, x61, x62, x63};
  this->SetLaserSelection(mask);
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetLaserSelection(int LaserSelection[64])
{
  for(int i = 0; i < 64; ++i)
    {
    this->Internal->LaserSelection[i] = LaserSelection[i];
    }
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::GetLaserSelection(int LaserSelection[64])
{
  for(int i = 0; i < 64; ++i)
    {
    LaserSelection[i] = this->Internal->LaserSelection[i];
    }
}


//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::GetVerticalCorrections(double VerticalCorrections[64])
{
  for(int i = 0; i < 64; ++i)
    {
    VerticalCorrections[i] = this->Internal->laser_corrections_[i].verticalCorrection;
    }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetDummyProperty(int vtkNotUsed(dummy))
{
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetPointsSkip(int pr)
{
  this->Internal->PointsSkip = pr;
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetNumberOfTrailingFrames(int numTrailing)
{
  assert(numTrailing >= 0);
  this->Internal->NumberOfTrailingFrames = numTrailing;
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetCorrectionsFile(const std::string& correctionsFile)
{
  if (correctionsFile == this->CorrectionsFile)
    {
    return;
    }

  if (correctionsFile.length())
    {
    this->Internal->LoadCorrectionsFile(correctionsFile);
    }
  else
    {
    this->Internal->LoadHDL32Corrections();
    }

  this->CorrectionsFile = correctionsFile;
  this->UnloadData();
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::UnloadData()
{
  for (size_t n = 0; n < HDL_MAX_NUM_LASERS; ++n)
    {
    this->Internal->LastPointId[n] = -1;
    }
  this->Internal->LastAzimuth = 0;
  this->Internal->Datasets.clear();
  this->Internal->CurrentDataset = this->Internal->CreateData(0);
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetTimestepInformation(vtkInformation *info)
{
  const size_t numberOfTimesteps = this->Internal->FilePositions.size();
  std::vector<double> timesteps;
  for (size_t i = 0; i < numberOfTimesteps; ++i)
    {
    timesteps.push_back(i);
    }

  if (numberOfTimesteps)
    {
    double timeRange[2] = {timesteps.front(), timesteps.back()};
    info->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(), &timesteps.front(), timesteps.size());
    info->Set(vtkStreamingDemandDrivenPipeline::TIME_RANGE(), timeRange, 2);
    }
  else
    {
    info->Remove(vtkStreamingDemandDrivenPipeline::TIME_STEPS());
    info->Remove(vtkStreamingDemandDrivenPipeline::TIME_RANGE());
    }
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::RequestData(vtkInformation *request,
                              vtkInformationVector **inputVector,
                              vtkInformationVector *outputVector)
{
  vtkPolyData *output = vtkPolyData::GetData(outputVector);
  vtkInformation *info = outputVector->GetInformationObject(0);

  if (!this->FileName.length())
    {
    vtkErrorMacro("FileName has not been set.");
    return 0;
    }


  int timestep = 0;
  if (info->Has(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP()))
    {
    double timeRequest = info->Get(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP());
    timestep = static_cast<int>(floor(timeRequest+0.5));
    }

  if (timestep < 0 || timestep >= this->GetNumberOfFrames())
    {
    vtkErrorMacro("Cannot meet timestep request: " << timestep << ".  Have " << this->GetNumberOfFrames() << " datasets.");
    output->ShallowCopy(this->Internal->CreateData(0));
    return 0;
    }

  this->Open();

  if(this->Internal->NumberOfTrailingFrames > 0)
    {
    output->ShallowCopy(this->GetFrameRange(timestep - this->Internal->NumberOfTrailingFrames,
                                            this->Internal->NumberOfTrailingFrames));
    }
  else
    {
    output->ShallowCopy(this->GetFrame(timestep));
    }
  this->Close();
  return 1;
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::RequestInformation(vtkInformation *request,
                                     vtkInformationVector **inputVector,
                                     vtkInformationVector *outputVector)
{
  if (this->FileName.length() && !this->Internal->FilePositions.size())
    {
    this->ReadFrameInformation();
    }

  vtkInformation *info = outputVector->GetInformationObject(0);
  this->SetTimestepInformation(info);
  return 1;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "FileName: " << this->FileName << endl;
  os << indent << "CorrectionsFile: " << this->CorrectionsFile << endl;
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::CanReadFile(const char *fname)
{
  return 1;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::ProcessHDLPacket(unsigned char *data, unsigned int bytesReceived)
{
  this->Internal->ProcessHDLPacket(data, bytesReceived);
}

//-----------------------------------------------------------------------------
std::vector<vtkSmartPointer<vtkPolyData> >& vtkVelodyneHDLReader::GetDatasets()
{
  return this->Internal->Datasets;
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::GetNumberOfFrames()
{
  return this->Internal->FilePositions.size();;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::Open()
{
  this->Close();
  this->Internal->Reader = new vtkPacketFileReader;
  if (!this->Internal->Reader->Open(this->FileName))
    {
    vtkErrorMacro("Failed to open packet file: " << this->FileName << endl << this->Internal->Reader->GetLastError());
    this->Close();
    }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::Close()
{
  delete this->Internal->Reader;
  this->Internal->Reader = 0;
}


//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::DumpFrames(int startFrame, int endFrame, const std::string& filename)
{
  if (!this->Internal->Reader)
    {
    vtkErrorMacro("DumpFrames() called but packet file reader is not open.");
    return;
    }

  vtkPacketFileWriter writer;
  if (!writer.Open(filename))
    {
    vtkErrorMacro("Failed to open packet file for writing: " << filename);
    return;
    }

  pcap_pkthdr* header = 0;
  const unsigned char* data = 0;
  unsigned int dataLength = 0;
  double timeSinceStart = 0;

  unsigned int lastAzimuth = 0;
  int currentFrame = startFrame;

  this->Internal->Reader->SetFilePosition(&this->Internal->FilePositions[startFrame]);
  int skip = this->Internal->Skips[startFrame];

  while (this->Internal->Reader->NextPacket(data, dataLength, timeSinceStart, &header) &&
         currentFrame <= endFrame)
    {
    if (dataLength == (1206 + 42))
      {
      writer.WritePacket(header, const_cast<unsigned char*>(data));
      }

    // Check if we cycled a frame and decrement
    const HDLDataPacket* dataPacket = reinterpret_cast<const HDLDataPacket *>(data + 42);

    for (int i = skip; i < HDL_FIRING_PER_PKT; ++i)
      {
      HDLFiringData firingData = dataPacket->firingData[i];

      if (firingData.rotationalPosition != 0 && firingData.rotationalPosition < lastAzimuth)
        {
        currentFrame++;
        if(currentFrame > endFrame)
          {
          break;
          }
        }
      lastAzimuth = firingData.rotationalPosition;
      }
    skip = 0;
    }

  writer.Close();
}

//-----------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> vtkVelodyneHDLReader::GetFrameRange(int startFrame, int numberOfFrames)
{
  this->UnloadData();
  if (!this->Internal->Reader)
    {
    vtkErrorMacro("GetFrame() called but packet file reader is not open.");
    return 0;
    }

  const unsigned char* data = 0;
  unsigned int dataLength = 0;
  double timeSinceStart = 0;

  if(startFrame < 0)
    {
    numberOfFrames -= startFrame;
    startFrame = 0;
    }
  assert(numberOfFrames > 0);

  this->Internal->Reader->SetFilePosition(&this->Internal->FilePositions[startFrame]);
  this->Internal->Skip = this->Internal->Skips[startFrame];

  this->Internal->SplitCounter = numberOfFrames;

  while (this->Internal->Reader->NextPacket(data, dataLength, timeSinceStart))
    {
    this->ProcessHDLPacket(const_cast<unsigned char*>(data), dataLength);

    if (this->Internal->Datasets.size())
      {
      this->Internal->SplitCounter = 0;
      return this->Internal->Datasets.back();
      }
    }

  this->Internal->SplitFrame(true);
  this->Internal->SplitCounter = 0;
  return this->Internal->Datasets.back();
}

//-----------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> vtkVelodyneHDLReader::GetFrame(int frameNumber)
{
  this->UnloadData();
  if (!this->Internal->Reader)
    {
    vtkErrorMacro("GetFrame() called but packet file reader is not open.");
    return 0;
    }

  assert(this->Internal->FilePositions.size() == this->Internal->Skips.size());
  if(frameNumber < 0 || frameNumber > this->Internal->FilePositions.size())
    {
    vtkErrorMacro("Invalid frame requested");
    return 0;
    }

  const unsigned char* data = 0;
  unsigned int dataLength = 0;
  double timeSinceStart = 0;


  this->Internal->Reader->SetFilePosition(&this->Internal->FilePositions[frameNumber]);
  this->Internal->Skip = this->Internal->Skips[frameNumber];

  while (this->Internal->Reader->NextPacket(data, dataLength, timeSinceStart))
    {
    this->ProcessHDLPacket(const_cast<unsigned char*>(data), dataLength);

    if (this->Internal->Datasets.size())
      {
      return this->Internal->Datasets.back();
      }
    }

  this->Internal->SplitFrame();
  return this->Internal->Datasets.back();
}

namespace
{
  template <typename T>
  T* CreateDataArray(const char* name, vtkIdType np, vtkPolyData* pd)
  {
    vtkSmartPointer<T> array = vtkSmartPointer<T>::New();
    array->Allocate(60000);
    array->SetName(name);
    array->SetNumberOfTuples(np);

    pd->GetPointData()->AddArray(array.GetPointer());

    return array.GetPointer();
  }
}

//-----------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> vtkVelodyneHDLReader::vtkInternal::CreateData(vtkIdType numberOfPoints)
{
  vtkSmartPointer<vtkPolyData> polyData = vtkSmartPointer<vtkPolyData>::New();

  // points
  vtkNew<vtkPoints> points;
  points->SetDataTypeToFloat();
  points->Allocate(60000);
  points->SetNumberOfPoints(numberOfPoints);
  polyData->SetPoints(points.GetPointer());
  polyData->SetVerts(NewVertexCells(numberOfPoints));

  // intensity
  this->Points = points.GetPointer();
  this->Intensity = CreateDataArray<vtkUnsignedCharArray>("intensity", numberOfPoints, polyData);
  this->LaserId = CreateDataArray<vtkUnsignedCharArray>("laser_id", numberOfPoints, polyData);
  this->Azimuth = CreateDataArray<vtkUnsignedShortArray>("azimuth", numberOfPoints, polyData);
  this->Distance = CreateDataArray<vtkDoubleArray>("distance_m", numberOfPoints, polyData);
  this->Timestamp = CreateDataArray<vtkUnsignedIntArray>("timestamp", numberOfPoints, polyData);
  this->Flags = CreateDataArray<vtkUnsignedIntArray>("dual_flags", numberOfPoints, polyData);
  this->IntensityDelta = CreateDataArray<vtkShortArray>("dual_intensity_difference", numberOfPoints, polyData);
  this->DistanceDelta = CreateDataArray<vtkDoubleArray>("dual_distance_difference", numberOfPoints, polyData);

  return polyData;
}

//----------------------------------------------------------------------------
vtkSmartPointer<vtkCellArray> vtkVelodyneHDLReader::vtkInternal::NewVertexCells(vtkIdType numberOfVerts)
{
  vtkNew<vtkIdTypeArray> cells;
  cells->SetNumberOfValues(numberOfVerts*2);
  vtkIdType* ids = cells->GetPointer(0);
  for (vtkIdType i = 0; i < numberOfVerts; ++i)
    {
    ids[i*2] = 1;
    ids[i*2+1] = i;
    }

  vtkSmartPointer<vtkCellArray> cellArray = vtkSmartPointer<vtkCellArray>::New();
  cellArray->SetCells(numberOfVerts, cells.GetPointer());
  return cellArray;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::PushFiringData(const unsigned char laserId,
                                                       unsigned short azimuth,
                                                       const unsigned int timestamp,
                                                       const HDLLaserReturn* laserReturn,
                                                       const HDLLaserCorrection* correction,
                                                       const unsigned short azimuthAdjustment,
                                                       const double translation[3],
                                                       const bool dualReturn)
{
  const vtkIdType thisPointId = this->Points->GetNumberOfPoints();
  const short intensity = laserReturn->intensity;

  this->Azimuth->InsertNextValue(azimuth);
  this->Intensity->InsertNextValue(laserReturn->intensity);
  this->LaserId->InsertNextValue(laserId);
  this->Timestamp->InsertNextValue(timestamp);

  azimuth = (azimuth + azimuthAdjustment) % 36000;

  double cosAzimuth, sinAzimuth;
  if (correction->azimuthCorrection == 0)
  {
    cosAzimuth = this->cos_lookup_table_[azimuth];
    sinAzimuth = this->sin_lookup_table_[azimuth];
  }
  else
  {
    double azimuthInRadians = HDL_Grabber_toRadians((static_cast<double> (azimuth) / 100.0) - correction->azimuthCorrection);
    cosAzimuth = std::cos (azimuthInRadians);
    sinAzimuth = std::sin (azimuthInRadians);
  }

  double distanceM = laserReturn->distance * 0.002 + correction->distanceCorrection;
  double xyDistance = distanceM * correction->cosVertCorrection - correction->sinVertOffsetCorrection;

  double x = (xyDistance * sinAzimuth - correction->horizontalOffsetCorrection * cosAzimuth);
  double y = (xyDistance * cosAzimuth + correction->horizontalOffsetCorrection * sinAzimuth);
  double z = (distanceM * correction->sinVertCorrection + correction->cosVertOffsetCorrection);

  x += translation[0];
  y += translation[1];
  z += translation[2];

  this->Points->InsertNextPoint(x,y,z);
  this->Distance->InsertNextValue(distanceM);

  if (dualReturn)
    {
    const vtkIdType dualPointId = this->LastPointId[laserId];
    if (dualPointId < this->FirstPointIdThisReturn)
      {
      // No matching point from first set (skipped?)
      this->Flags->InsertNextValue(DUAL_DOUBLED);
      this->IntensityDelta->InsertNextValue(0);
      this->DistanceDelta->InsertNextValue(0.0);
      }
    else
      {
      const short dualIntensity = this->Intensity->GetValue(dualPointId);
      const double dualDistance = this->Distance->GetValue(dualPointId);
      unsigned int firstFlags = this->Flags->GetValue(dualPointId);
      unsigned int secondFlags = 0;

      if (dualIntensity < intensity)
        {
        firstFlags &= ~DUAL_INTENSITY_HIGH;
        secondFlags |= DUAL_INTENSITY_HIGH;
        }
      else
        {
        firstFlags &= ~DUAL_INTENSITY_LOW;
        secondFlags |= DUAL_INTENSITY_LOW;
        }

      if (dualDistance < distanceM)
        {
        firstFlags &= ~DUAL_DISTANCE_FAR;
        secondFlags |= DUAL_DISTANCE_FAR;
        }
      else
        {
        firstFlags &= ~DUAL_DISTANCE_NEAR;
        secondFlags |= DUAL_DISTANCE_NEAR;
        }

      this->Flags->SetValue(dualPointId, firstFlags);
      this->Flags->InsertNextValue(secondFlags);

      const short meanIntensity = dualIntensity + intensity;
      const double meanDistance = 0.5 * (dualDistance + distanceM);

      this->IntensityDelta->SetValue(dualPointId, (2 * dualIntensity) - meanIntensity);
      this->IntensityDelta->InsertNextValue((2 * intensity) - meanIntensity);

      this->DistanceDelta->SetValue(dualPointId, dualDistance - meanDistance);
      this->DistanceDelta->InsertNextValue(distanceM - meanDistance);
      }
    }
  else
    {
    this->Flags->InsertNextValue(DUAL_DOUBLED);
    this->IntensityDelta->InsertNextValue(0);
    this->DistanceDelta->InsertNextValue(0.0);
    }

  this->LastPointId[laserId] = thisPointId;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::InitTables()
{
  if (cos_lookup_table_ == NULL && sin_lookup_table_ == NULL)
    {
    cos_lookup_table_ = new double[HDL_NUM_ROT_ANGLES];
    sin_lookup_table_ = new double[HDL_NUM_ROT_ANGLES];
    for (unsigned int i = 0; i < HDL_NUM_ROT_ANGLES; i++)
      {
      double rad = HDL_Grabber_toRadians(i / 100.0);
      cos_lookup_table_[i] = std::cos(rad);
      sin_lookup_table_[i] = std::sin(rad);
      }
    }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::LoadCorrectionsFile(const std::string& correctionsFile)
{

  boost::property_tree::ptree pt;
  try
    {
    read_xml(correctionsFile, pt, boost::property_tree::xml_parser::trim_whitespace);
    }
  catch (boost::exception const&)
    {
    vtkGenericWarningMacro("LoadCorrectionsFile: error reading calibration file: " << correctionsFile);
    return;
    }

  BOOST_FOREACH (boost::property_tree::ptree::value_type &v, pt.get_child("boost_serialization.DB.points_"))
    {
    if (v.first == "item")
      {
      boost::property_tree::ptree points = v.second;
      BOOST_FOREACH (boost::property_tree::ptree::value_type &px, points)
        {
        if (px.first == "px")
          {
          boost::property_tree::ptree calibrationData = px.second;
          int index = -1;
          double azimuth = 0;
          double vertCorrection = 0;
          double distCorrection = 0;
          double vertOffsetCorrection = 0;
          double horizOffsetCorrection = 0;

          BOOST_FOREACH (boost::property_tree::ptree::value_type &item, calibrationData)
            {
            if (item.first == "id_")
              index = atoi(item.second.data().c_str());
            if (item.first == "rotCorrection_")
              azimuth = atof(item.second.data().c_str());
            if (item.first == "vertCorrection_")
              vertCorrection = atof(item.second.data().c_str());
            if (item.first == "distCorrection_")
              distCorrection = atof(item.second.data().c_str());
            if (item.first == "vertOffsetCorrection_")
              vertOffsetCorrection = atof(item.second.data().c_str());
            if (item.first == "horizOffsetCorrection_")
              horizOffsetCorrection = atof(item.second.data().c_str());
            }
          if (index != -1)
            {
            laser_corrections_[index].azimuthCorrection = azimuth;
            laser_corrections_[index].verticalCorrection = vertCorrection;
            laser_corrections_[index].distanceCorrection = distCorrection / 100.0;
            laser_corrections_[index].verticalOffsetCorrection = vertOffsetCorrection / 100.0;
            laser_corrections_[index].horizontalOffsetCorrection = horizOffsetCorrection / 100.0;

            laser_corrections_[index].cosVertCorrection = std::cos (HDL_Grabber_toRadians(laser_corrections_[index].verticalCorrection));
            laser_corrections_[index].sinVertCorrection = std::sin (HDL_Grabber_toRadians(laser_corrections_[index].verticalCorrection));
            }
          }
        }
      }
    }

  this->SetCorrectionsCommon();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::LoadHDL32Corrections()
{
  double hdl32VerticalCorrections[] = {
    -30.67, -9.3299999, -29.33, -8, -28,
    -6.6700001, -26.67, -5.3299999, -25.33, -4, -24, -2.6700001, -22.67,
    -1.33, -21.33, 0, -20, 1.33, -18.67, 2.6700001, -17.33, 4, -16, 5.3299999,
    -14.67, 6.6700001, -13.33, 8, -12, 9.3299999, -10.67, 10.67 };

  for (int i = 0; i < HDL_LASER_PER_FIRING; i++)
    {
    laser_corrections_[i].azimuthCorrection = 0.0;
    laser_corrections_[i].distanceCorrection = 0.0;
    laser_corrections_[i].horizontalOffsetCorrection = 0.0;
    laser_corrections_[i].verticalOffsetCorrection = 0.0;
    laser_corrections_[i].verticalCorrection = hdl32VerticalCorrections[i];
    laser_corrections_[i].sinVertCorrection = std::sin (HDL_Grabber_toRadians(hdl32VerticalCorrections[i]));
    laser_corrections_[i].cosVertCorrection = std::cos (HDL_Grabber_toRadians(hdl32VerticalCorrections[i]));
    }

  for (int i = HDL_LASER_PER_FIRING; i < HDL_MAX_NUM_LASERS; i++)
    {
    laser_corrections_[i].azimuthCorrection = 0.0;
    laser_corrections_[i].distanceCorrection = 0.0;
    laser_corrections_[i].horizontalOffsetCorrection = 0.0;
    laser_corrections_[i].verticalOffsetCorrection = 0.0;
    laser_corrections_[i].verticalCorrection = 0.0;
    laser_corrections_[i].sinVertCorrection = 0.0;
    laser_corrections_[i].cosVertCorrection = 1.0;
    }

  this->SetCorrectionsCommon();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::SetCorrectionsCommon()
{
  for (int i = 0; i < HDL_MAX_NUM_LASERS; i++)
    {
    HDLLaserCorrection correction = laser_corrections_[i];
    laser_corrections_[i].sinVertOffsetCorrection = correction.verticalOffsetCorrection
                                       * correction.sinVertCorrection;
    laser_corrections_[i].cosVertOffsetCorrection = correction.verticalOffsetCorrection
                                       * correction.cosVertCorrection;
    }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::Init()
{
  this->InitTables();
  this->LoadHDL32Corrections();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::SplitFrame(bool force)
{
  if(this->SplitCounter > 0 && !force)
    {
    this->SplitCounter--;
    return;
    }

  for (size_t n = 0; n < HDL_MAX_NUM_LASERS; ++n)
    {
    this->LastPointId[n] = -1;
    }

  this->CurrentDataset->SetVerts(this->NewVertexCells(this->CurrentDataset->GetNumberOfPoints()));
  this->Datasets.push_back(this->CurrentDataset);
  this->CurrentDataset = this->CreateData(0);
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::ComputeOrientation(unsigned int timestamp,
                                                           unsigned int& azimuthOffset,
                                                           double translation[3])
{
  if(this->ApplyTransform && this->Interp)
    {
    double tuple[5];
    this->Interp->InterpolateTuple(timestamp, tuple);

    double angle = std::atan2(tuple[4], tuple[3]);
    angle = (angle > 0 ? angle : (2*vtkMath::Pi() + angle));
    angle = 180 * angle / vtkMath::Pi();

    azimuthOffset = static_cast<unsigned int>(angle * 100);

    translation[0] = tuple[0];
    translation[1] = tuple[1];
    translation[2] = tuple[2];
    }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::ProcessFiring(HDLFiringData* firingData,
                                                      int offset,
                                                      unsigned int gpsTime,
                                                      unsigned int azimuthOffset,
                                                      const double translation[3])
{
  const bool dual = (this->LastAzimuth == firingData->rotationalPosition);
  if (!dual)
    {
    this->FirstPointIdThisReturn = this->Points->GetNumberOfPoints();
    }

  for (int j = 0; j < HDL_LASER_PER_FIRING; j++)
    {
    unsigned char laserId = static_cast<unsigned char>(j + offset);
    if (firingData->laserReturns[j].distance != 0.0 && this->LaserSelection[laserId])
      {
      this->PushFiringData(laserId,
                           firingData->rotationalPosition,
                           gpsTime,
                           &(firingData->laserReturns[j]),
                           &(laser_corrections_[j + offset]),
                           azimuthOffset,
                           translation,
                           dual);
      }
    }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::ProcessHDLPacket(unsigned char *data, std::size_t bytesReceived)
{
  if (bytesReceived != 1206)
    {
    return;
    }

  HDLDataPacket* dataPacket = reinterpret_cast<HDLDataPacket *>(data);

  unsigned int azimuthOffset = 0;
  double translation[3] = {0.0, 0.0, 0.0};
  this->ComputeOrientation(dataPacket->gpsTimestamp,
                           azimuthOffset,
                           translation);

  int i = this->Skip;
  this->Skip = 0;

  for ( ; i < HDL_FIRING_PER_PKT; ++i)
    {
    HDLFiringData* firingData = &(dataPacket->firingData[i]);
    int offset = (firingData->blockIdentifier == BLOCK_0_TO_31) ? 0 : 32;

    if (firingData->rotationalPosition < this->LastAzimuth)
      {
      this->SplitFrame();
      }

    // Skip this firing every PointSkip
    if(this->PointsSkip == 0 || i % (this->PointsSkip + 1) == 0)
      {
      this->ProcessFiring(firingData,
                          offset,
                          dataPacket->gpsTimestamp,
                          azimuthOffset,
                          translation);
      }

    this->LastAzimuth = firingData->rotationalPosition;
    }
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::ReadFrameInformation()
{
  vtkPacketFileReader reader;
  if (!reader.Open(this->FileName))
    {
    vtkErrorMacro("Failed to open packet file: " << this->FileName << endl << reader.GetLastError());
    return 0;
    }

  const unsigned char* data = 0;
  unsigned int dataLength = 0;
  double timeSinceStart = 0;

  unsigned int lastAzimuth = 0;
  unsigned int lastTimestamp = 0;

  std::vector<fpos_t> filePositions;
  std::vector<int> skips;

  fpos_t lastFilePosition;
  reader.GetFilePosition(&lastFilePosition);


  filePositions.push_back(lastFilePosition);
  skips.push_back(0);

  while (reader.NextPacket(data, dataLength, timeSinceStart))
    {

    if (dataLength != 1206)
      {
      continue;
      }

    const HDLDataPacket* dataPacket = reinterpret_cast<const HDLDataPacket *>(data);

    unsigned int timeDiff = dataPacket->gpsTimestamp - lastTimestamp;
    if (timeDiff > 600 && lastTimestamp != 0)
      {
      printf("missed %d packets\n",  static_cast<int>(floor((timeDiff/553.0) + 0.5)));
      }

    for (int i = 0; i < HDL_FIRING_PER_PKT; ++i)
      {
      HDLFiringData firingData = dataPacket->firingData[i];

      if (firingData.rotationalPosition < lastAzimuth)
        {
        filePositions.push_back(lastFilePosition);
        skips.push_back(i);
        this->UpdateProgress(0.0);
        }

      lastAzimuth = firingData.rotationalPosition;
      }

    lastTimestamp = dataPacket->gpsTimestamp;
    reader.GetFilePosition(&lastFilePosition);
    }

  this->Internal->FilePositions = filePositions;
  this->Internal->Skips = skips;
  return this->GetNumberOfFrames();
}
