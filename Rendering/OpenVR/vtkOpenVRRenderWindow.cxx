/*=========================================================================

Program:   Visualization Toolkit
Module:    vtkOpenVRRenderWindow.cxx

Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
All rights reserved.
See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

This software is distributed WITHOUT ANY WARRANTY; without even
the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE.  See the above copyright notice for more information.

Parts Copyright Valve Coproration from hellovr_opengl_main.cpp
under their BSD license found here:
https://github.com/ValveSoftware/openvr/blob/master/LICENSE

=========================================================================*/
#include "vtkOpenVRRenderWindow.h"

#include "vtkCommand.h"
#include "vtkFloatArray.h"
#include "vtkIdList.h"
#include "vtkMath.h"
#include "vtkMatrix4x4.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkOpenGLError.h"
#include "vtkOpenGLIndexBufferObject.h"
#include "vtkOpenGLRenderWindow.h"
#include "vtkOpenGLRenderer.h"
#include "vtkOpenGLShaderCache.h"
#include "vtkOpenGLState.h"
#include "vtkOpenGLTexture.h"
#include "vtkOpenGLVertexArrayObject.h"
#include "vtkOpenGLVertexBufferObject.h"
#include "vtkOpenVRCamera.h"
#include "vtkOpenVRDefaultOverlay.h"
#include "vtkOpenVRModel.h"
#include "vtkOpenVRRenderWindowInteractor.h"
#include "vtkOpenVRRenderer.h"
#include "vtkPointData.h"
#include "vtkPolyData.h"
#include "vtkPolyDataMapper.h"
#include "vtkRenderWindowInteractor.h"
#include "vtkRendererCollection.h"
#include "vtkShaderProgram.h"
#include "vtkSmartPointer.h"
#include "vtkTextureObject.h"
#include "vtkTransform.h"

#include <cmath>
#include <sstream>

vtkStandardNewMacro(vtkOpenVRRenderWindow);

//------------------------------------------------------------------------------
vtkOpenVRRenderWindow::vtkOpenVRRenderWindow()
{
  this->DashboardOverlay = vtkSmartPointer<vtkOpenVRDefaultOverlay>::New();
}

//------------------------------------------------------------------------------
vtkRenderWindowInteractor* vtkOpenVRRenderWindow::MakeRenderWindowInteractor()
{
  this->Interactor = vtkSmartPointer<vtkOpenVRRenderWindowInteractor>::New();
  this->Interactor->SetRenderWindow(this);
  return this->Interactor;
}

//------------------------------------------------------------------------------
std::string vtkOpenVRRenderWindow::GetTrackedDeviceString(vr::IVRSystem* pHmd,
  vr::TrackedDeviceIndex_t unDevice, vr::TrackedDeviceProperty prop,
  vr::TrackedPropertyError* peError)
{
  uint32_t unRequiredBufferLen =
    pHmd->GetStringTrackedDeviceProperty(unDevice, prop, nullptr, 0, peError);
  if (unRequiredBufferLen == 0)
  {
    return "";
  }

  char* pchBuffer = new char[unRequiredBufferLen];
  unRequiredBufferLen =
    pHmd->GetStringTrackedDeviceProperty(unDevice, prop, pchBuffer, unRequiredBufferLen, peError);
  std::string sResult = pchBuffer;
  delete[] pchBuffer;
  return sResult;
}

//------------------------------------------------------------------------------
vtkOpenVRModel* vtkOpenVRRenderWindow::FindOrLoadRenderModel(const char* pchRenderModelName)
{
  // create the model
  vtkOpenVRModel* pRenderModel = vtkOpenVRModel::New();
  pRenderModel->SetName(pchRenderModelName);

  // start loading the model
  auto status = vr::VRRenderModels()->LoadRenderModel_Async(
    pRenderModel->GetName().c_str(), &pRenderModel->RawModel);
  if (status == vr::EVRRenderModelError::VRRenderModelError_NoShapes)
  {
    pRenderModel->SetVisibility(false);
    return pRenderModel;
  }

  if (status > vr::EVRRenderModelError::VRRenderModelError_Loading)
  {
    vtkErrorMacro(
      "Unable to load render model " << pRenderModel->GetName() << " with status " << status);
    pRenderModel->Delete();
    return nullptr; // move on to the next tracked device
  }

  pRenderModel->SetVisibility(true);
  return pRenderModel;
}

//------------------------------------------------------------------------------
void vtkOpenVRRenderWindow::RenderModels()
{
  vtkOpenGLState* ostate = this->GetState();
  ostate->vtkglEnable(GL_DEPTH_TEST);

  // for each device
  for (uint32_t unTrackedDevice = vr::k_unTrackedDeviceIndex_Hmd + 1;
       unTrackedDevice < vr::k_unMaxTrackedDeviceCount; unTrackedDevice++)
  {
    // is it not connected?
    if (!this->HMD->IsTrackedDeviceConnected(unTrackedDevice))
    {
      continue;
    }

    if (!this->BaseStationVisibility &&
      this->HMD->GetTrackedDeviceClass(unTrackedDevice) == vr::TrackedDeviceClass_TrackingReference)
    {
      continue;
    }

    // do we not have a model loaded yet? try loading one
    auto handle = this->GetDeviceHandleForOpenVRHandle(unTrackedDevice);
    auto* pRenderModel = this->GetModelForDeviceHandle(handle);
    if (!pRenderModel)
    {
      std::string sRenderModelName =
        this->GetTrackedDeviceString(this->HMD, unTrackedDevice, vr::Prop_RenderModelName_String);
      vtkSmartPointer<vtkOpenVRModel> newModel;
      newModel.TakeReference(this->FindOrLoadRenderModel(sRenderModelName.c_str()));
      if (newModel)
      {
        this->SetModelForDeviceHandle(handle, newModel);
      }
      pRenderModel = newModel;
    }
    // if we have a model and it is visible
    if (pRenderModel && pRenderModel->GetVisibility())
    {
      vtkMatrix4x4* tdPose = this->GetDeviceToPhysicalMatrixForDeviceHandle(handle);
      pRenderModel->Render(this, tdPose);
    }
  }
}

//------------------------------------------------------------------------------
void vtkOpenVRRenderWindow::UpdateHMDMatrixPose()
{
  if (!this->HMD)
  {
    return;
  }

  // Retrieve Open VR poses
  vr::TrackedDevicePose_t OpenVRTrackedDevicePoses[vr::k_unMaxTrackedDeviceCount];
  vr::VRCompositor()->WaitGetPoses(
    OpenVRTrackedDevicePoses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);

  // Store poses with generic type
  for (uint32_t deviceIdx = 0; deviceIdx < vr::k_unMaxTrackedDeviceCount; ++deviceIdx)
  {
    if (OpenVRTrackedDevicePoses[deviceIdx].bPoseIsValid)
    {
      auto handle = this->GetDeviceHandleForOpenVRHandle(deviceIdx);
      auto device = this->GetDeviceForOpenVRHandle(deviceIdx);
      this->AddDeviceHandle(handle, device);
      vtkMatrix4x4* tdPose = this->GetDeviceToPhysicalMatrixForDeviceHandle(handle);
      this->CreateMatrixFromVrPose(tdPose, OpenVRTrackedDevicePoses[deviceIdx]);
    }
  }

  // update the camera values based on the pose
  auto hmdHandle = this->GetDeviceHandleForOpenVRHandle(vr::k_unTrackedDeviceIndex_Hmd);

  vtkRenderer* ren;
  vtkCollectionSimpleIterator rit;
  this->Renderers->InitTraversal(rit);
  while ((ren = this->Renderers->GetNextRenderer(rit)))
  {
    vtkOpenVRCamera* cam = vtkOpenVRCamera::SafeDownCast(ren->GetActiveCamera());

    // get the position and orientation of the HMD
    vtkMatrix4x4* tdPose = this->GetDeviceToPhysicalMatrixForDeviceHandle(hmdHandle);

    // Note: Scaling is applied through moving the camera closer to the focal point, because
    // scaling of all actors is not feasible, and vtkCamera::ModelTransformMatrix is not supported
    // throughout VTK (clipping issues etc.). To achieve this, a new coordinate system called
    // NonScaledWorld is introduced. The relationship between Physical (in which the HMD pose
    // is given by OpenVR) and NonScaledWorld is described by the PhysicalViewUp etc. member
    // variables. After getting the HMD pose in Physical, those coordinates and axes are converted
    // to the NonScaledWorld coordinate system, on which the PhysicalScaling trick of modifying
    // the camera position is applied, resulting the World coordinate system.

    // construct physical to non-scaled world axes (scaling is used later to move camera closer)
    double physicalZ_NonscaledWorld[3] = { -this->PhysicalViewDirection[0],
      -this->PhysicalViewDirection[1], -this->PhysicalViewDirection[2] };
    double* physicalY_NonscaledWorld = this->PhysicalViewUp;
    double physicalX_NonscaledWorld[3] = { 0.0 };
    vtkMath::Cross(physicalY_NonscaledWorld, physicalZ_NonscaledWorld, physicalX_NonscaledWorld);

    // extract HMD axes and position
    double hmdX_Physical[3] = { tdPose->GetElement(0, 0), tdPose->GetElement(1, 0),
      tdPose->GetElement(2, 0) };
    double hmdY_Physical[3] = { tdPose->GetElement(0, 1), tdPose->GetElement(1, 1),
      tdPose->GetElement(2, 1) };
    double hmdPosition_Physical[3] = { tdPose->GetElement(0, 3), tdPose->GetElement(1, 3),
      tdPose->GetElement(2, 3) };

    // convert position to non-scaled world coordinates
    double hmdPosition_NonscaledWorld[3];
    hmdPosition_NonscaledWorld[0] = hmdPosition_Physical[0] * physicalX_NonscaledWorld[0] +
      hmdPosition_Physical[1] * physicalY_NonscaledWorld[0] +
      hmdPosition_Physical[2] * physicalZ_NonscaledWorld[0];
    hmdPosition_NonscaledWorld[1] = hmdPosition_Physical[0] * physicalX_NonscaledWorld[1] +
      hmdPosition_Physical[1] * physicalY_NonscaledWorld[1] +
      hmdPosition_Physical[2] * physicalZ_NonscaledWorld[1];
    hmdPosition_NonscaledWorld[2] = hmdPosition_Physical[0] * physicalX_NonscaledWorld[2] +
      hmdPosition_Physical[1] * physicalY_NonscaledWorld[2] +
      hmdPosition_Physical[2] * physicalZ_NonscaledWorld[2];
    // now adjust for scale and translation
    double hmdPosition_World[3] = { 0.0 };
    for (int i = 0; i < 3; i++)
    {
      hmdPosition_World[i] =
        hmdPosition_NonscaledWorld[i] * this->PhysicalScale - this->PhysicalTranslation[i];
    }

    // convert axes to non-scaled world coordinate system
    double hmdX_NonscaledWorld[3] = { hmdX_Physical[0] * physicalX_NonscaledWorld[0] +
        hmdX_Physical[1] * physicalY_NonscaledWorld[0] +
        hmdX_Physical[2] * physicalZ_NonscaledWorld[0],
      hmdX_Physical[0] * physicalX_NonscaledWorld[1] +
        hmdX_Physical[1] * physicalY_NonscaledWorld[1] +
        hmdX_Physical[2] * physicalZ_NonscaledWorld[1],
      hmdX_Physical[0] * physicalX_NonscaledWorld[2] +
        hmdX_Physical[1] * physicalY_NonscaledWorld[2] +
        hmdX_Physical[2] * physicalZ_NonscaledWorld[2] };
    double hmdY_NonscaledWorld[3] = { hmdY_Physical[0] * physicalX_NonscaledWorld[0] +
        hmdY_Physical[1] * physicalY_NonscaledWorld[0] +
        hmdY_Physical[2] * physicalZ_NonscaledWorld[0],
      hmdY_Physical[0] * physicalX_NonscaledWorld[1] +
        hmdY_Physical[1] * physicalY_NonscaledWorld[1] +
        hmdY_Physical[2] * physicalZ_NonscaledWorld[1],
      hmdY_Physical[0] * physicalX_NonscaledWorld[2] +
        hmdY_Physical[1] * physicalY_NonscaledWorld[2] +
        hmdY_Physical[2] * physicalZ_NonscaledWorld[2] };
    double hmdZ_NonscaledWorld[3] = { 0.0 };
    vtkMath::Cross(hmdY_NonscaledWorld, hmdX_NonscaledWorld, hmdZ_NonscaledWorld);

    cam->SetPosition(hmdPosition_World);
    cam->SetFocalPoint(hmdPosition_World[0] + hmdZ_NonscaledWorld[0] * this->PhysicalScale,
      hmdPosition_World[1] + hmdZ_NonscaledWorld[1] * this->PhysicalScale,
      hmdPosition_World[2] + hmdZ_NonscaledWorld[2] * this->PhysicalScale);
    cam->SetViewUp(hmdY_NonscaledWorld);

    ren->UpdateLightsGeometryToFollowCamera();
  }
}

//------------------------------------------------------------------------------
void vtkOpenVRRenderWindow::CreateMatrixFromVrPose(
  vtkMatrix4x4* result, const vr::TrackedDevicePose_t& vrPose)
{
  const float(*openVRPose)[4] = vrPose.mDeviceToAbsoluteTracking.m;

  for (vtkIdType i = 0; i < 3; ++i)
  {
    for (vtkIdType j = 0; j < 4; ++j)
    {
      result->SetElement(i, j, openVRPose[i][j]);
    }
  }

  // Add last row
  result->SetElement(3, 0, 0.0);
  result->SetElement(3, 1, 0.0);
  result->SetElement(3, 2, 0.0);
  result->SetElement(3, 3, 1.0);
}

//------------------------------------------------------------------------------
void vtkOpenVRRenderWindow::Render()
{
  if (this->TrackHMD)
  {
    this->UpdateHMDMatrixPose();
  }
  else
  {
    // Retrieve OpenVR poses
    vr::TrackedDevicePose_t OpenVRTrackedDevicePoses[vr::k_unMaxTrackedDeviceCount];
    vr::VRCompositor()->WaitGetPoses(
      OpenVRTrackedDevicePoses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);

    // Store poses with generic type
    for (uint32_t deviceIdx = 0; deviceIdx < vr::k_unMaxTrackedDeviceCount; ++deviceIdx)
    {
      if (OpenVRTrackedDevicePoses[deviceIdx].bPoseIsValid)
      {
        auto handle = this->GetDeviceHandleForOpenVRHandle(deviceIdx);
        auto device = this->GetDeviceForOpenVRHandle(deviceIdx);
        this->AddDeviceHandle(handle, device);
        vtkMatrix4x4* tdPose = this->GetDeviceToPhysicalMatrixForDeviceHandle(handle);
        this->CreateMatrixFromVrPose(tdPose, OpenVRTrackedDevicePoses[deviceIdx]);
      }
    }
  }

  this->Superclass::Render();
}

//------------------------------------------------------------------------------
void vtkOpenVRRenderWindow::StereoMidpoint()
{
  // render the left eye models
  this->RenderModels();

  this->GetState()->vtkglDisable(GL_MULTISAMPLE);

  if (this->HMD && this->SwapBuffers) // picking does not swap and we don't show it
  {
    this->RenderFramebuffer(this->FramebufferDescs[vtkVRRenderWindow::LeftEye]);

    vr::Texture_t leftEyeTexture = {
      (void*)(long)this->FramebufferDescs[vtkVRRenderWindow::LeftEye].ResolveColorTextureId,
      vr::TextureType_OpenGL, vr::ColorSpace_Gamma
    };
    vr::VRCompositor()->Submit(vr::Eye_Left, &leftEyeTexture);
  }
}

//------------------------------------------------------------------------------
void vtkOpenVRRenderWindow::StereoRenderComplete()
{
  // render the right eye models
  this->RenderModels();

  // reset the camera to a neutral position
  vtkRenderer* ren = vtkRenderer::SafeDownCast(this->GetRenderers()->GetItemAsObject(0));
  if (ren && !ren->GetSelector())
  {
    vtkOpenVRCamera* cam = vtkOpenVRCamera::SafeDownCast(ren->GetActiveCamera());
    cam->ApplyEyePose(this, false, -1.0);
  }

  this->GetState()->vtkglDisable(GL_MULTISAMPLE);

  // for now as fast as possible
  if (this->HMD && this->SwapBuffers) // picking does not swap and we don't show it
  {
    this->RenderFramebuffer(this->FramebufferDescs[vtkVRRenderWindow::RightEye]);

    vr::Texture_t rightEyeTexture = {
      (void*)(long)this->FramebufferDescs[vtkVRRenderWindow::RightEye].ResolveColorTextureId,
      vr::TextureType_OpenGL, vr::ColorSpace_Gamma
    };
    vr::VRCompositor()->Submit(vr::Eye_Right, &rightEyeTexture);
  }
}

//------------------------------------------------------------------------------
bool vtkOpenVRRenderWindow::CreateFramebuffers(uint32_t viewCount)
{
  this->FramebufferDescs.resize(viewCount);
  for (uint32_t vc = 0; vc < viewCount; vc++)
  {
    if (!this->CreateOneFramebuffer(this->Size[0], this->Size[1], this->FramebufferDescs[vc]))
    {
      return false;
    }
  }

  return true;
}

//------------------------------------------------------------------------------
bool vtkOpenVRRenderWindow::CreateOneFramebuffer(
  int nWidth, int nHeight, FramebufferDesc& framebufferDesc)
{
  glGenFramebuffers(1, &framebufferDesc.ResolveFramebufferId);
  this->GetState()->vtkglBindFramebuffer(GL_FRAMEBUFFER, framebufferDesc.ResolveFramebufferId);

  glGenTextures(1, &framebufferDesc.ResolveColorTextureId);
  glBindTexture(GL_TEXTURE_2D, framebufferDesc.ResolveColorTextureId);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nWidth, nHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glFramebufferTexture2D(
    GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, framebufferDesc.ResolveColorTextureId, 0);

  // check FBO status
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE)
  {
    return false;
  }

  this->GetState()->vtkglBindFramebuffer(GL_FRAMEBUFFER, 0);

  return true;
}

//------------------------------------------------------------------------------
bool vtkOpenVRRenderWindow::IsHMDPresent()
{
  // Returns true if the system believes that an HMD is present on the system. This function is much
  // faster than initializing all of OpenVR just to check for an HMD. Use it when you have a piece
  // of UI that you want to enable only for users with an HMD.
  return vr::VR_IsHmdPresent();
}

//------------------------------------------------------------------------------
bool vtkOpenVRRenderWindow::GetSizeFromAPI()
{
  if (!this->HMD)
  {
    return false;
  }

  uint32_t renderWidth;
  uint32_t renderHeight;
  this->HMD->GetRecommendedRenderTargetSize(&renderWidth, &renderHeight);

  this->Size[0] = renderWidth;
  this->Size[1] = renderHeight;

  return true;
}

//------------------------------------------------------------------------------
std::string vtkOpenVRRenderWindow::GetWindowTitleFromAPI()
{
  std::string strDriver = "No Driver";
  std::string strDisplay = "No Display";

  strDriver = this->GetTrackedDeviceString(
    this->HMD, vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String);
  strDisplay = this->GetTrackedDeviceString(
    this->HMD, vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SerialNumber_String);

  return "VTK -" + strDriver + " " + strDisplay;
}

//------------------------------------------------------------------------------
void vtkOpenVRRenderWindow::Initialize()
{
  // Loading the SteamVR Runtime
  vr::EVRInitError eError = vr::VRInitError_None;
  this->HMD = vr::VR_Init(&eError, vr::VRApplication_Scene);

  if (eError != vr::VRInitError_None)
  {
    this->HMD = nullptr;
    char buf[1024];
    snprintf(buf, sizeof(buf), "Unable to init VR runtime: %s",
      vr::VR_GetVRInitErrorAsEnglishDescription(eError));
    vtkErrorMacro(<< "VR_Init Failed" << buf);
    return;
  }

  this->OpenVRRenderModels =
    (vr::IVRRenderModels*)vr::VR_GetGenericInterface(vr::IVRRenderModels_Version, &eError);
  if (!this->OpenVRRenderModels)
  {
    this->HMD = nullptr;
    vr::VR_Shutdown();

    char buf[1024];
    snprintf(buf, sizeof(buf), "Unable to get render model interface: %s",
      vr::VR_GetVRInitErrorAsEnglishDescription(eError));
    vtkErrorMacro(<< "VR_Init Failed" << buf);
    return;
  }

  // Initialize the helper window and OpenGL through the superclass
  // This will also call CreateFramebuffers
  this->Superclass::Initialize();

  if (!this->Initialized)
  {
    vtkErrorMacro("VRRenderWindow initialization failed.");
    return;
  }

  if (!vr::VRCompositor())
  {
    vtkErrorMacro("Compositor initialization failed.");
    return;
  }

  this->DashboardOverlay->Create(this);
}

//------------------------------------------------------------------------------
void vtkOpenVRRenderWindow::ReleaseGraphicsResources(vtkWindow* renWin)
{
  this->Superclass::ReleaseGraphicsResources(renWin);

  if (this->HMD)
  {
    vr::VR_Shutdown();
    this->HMD = nullptr;
  }
}

//------------------------------------------------------------------------------
void vtkOpenVRRenderWindow::RenderOverlay()
{
  this->DashboardOverlay->Render();
}

//------------------------------------------------------------------------------
uint32_t vtkOpenVRRenderWindow::GetDeviceHandleForOpenVRHandle(vr::TrackedDeviceIndex_t index)
{
  return static_cast<uint32_t>(index);
}

vtkEventDataDevice vtkOpenVRRenderWindow::GetDeviceForOpenVRHandle(vr::TrackedDeviceIndex_t ohandle)
{
  if (ohandle == vr::k_unTrackedDeviceIndex_Hmd)
  {
    return vtkEventDataDevice::HeadMountedDisplay;
  }
  if (ohandle ==
    this->HMD->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand))
  {
    return vtkEventDataDevice::LeftController;
  }
  if (ohandle ==
    this->HMD->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand))
  {
    return vtkEventDataDevice::RightController;
  }

  {
    bool notDone = true;
    uint32_t arraySize(1024);
    vr::TrackedDeviceIndex_t* devices = new vr::TrackedDeviceIndex_t[arraySize];
    uint32_t deviceCount(0);
    while (notDone)
    {
      deviceCount = this->HMD->GetSortedTrackedDeviceIndicesOfClass(
        vr::TrackedDeviceClass_GenericTracker, devices, 1024);
      if (deviceCount > arraySize)
      {
        delete[] devices;
        arraySize *= 2;
        devices = new vr::TrackedDeviceIndex_t[arraySize];
        continue;
      }
      else
      {
        notDone = false;
      }
    }

    for (uint32_t i = 0; i < arraySize; i++)
    {
      if (devices[i] == ohandle)
      {
        delete[] devices;
        return vtkEventDataDevice::GenericTracker;
      }
    }
    delete[] devices;
  }

  return vtkEventDataDevice::Unknown;
}
