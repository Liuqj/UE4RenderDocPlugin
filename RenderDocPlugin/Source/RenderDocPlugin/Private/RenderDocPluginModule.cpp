/******************************************************************************
* The MIT License (MIT)
*
* Copyright (c) 2014 Fredrik Lindh
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
******************************************************************************/

#include "RenderDocPluginPrivatePCH.h" 
#include "RendererInterface.h"
#include "RenderDocPluginModule.h"
#include "RenderDocPluginNotification.h"

const FName FRenderDocPluginModule::SettingsUITabName(TEXT("RenderDocSettingsUI"));

#define LOCTEXT_NAMESPACE "RenderDocPlugin"

void FRenderDocPluginModule::StartupModule()
{
	if (GUsingNullRHI)
	{
		UE_LOG(RenderDocPlugin, Warning, TEXT("RenderDoc Plugin will not be loaded because a Null RHI (Cook Server, perhaps) is being used."));
		return;
	}

	//Load DLL
	FString BinaryPath;
	if (GConfig)
	{
		GConfig->GetString(TEXT("RenderDoc"), TEXT("BinaryPath"), BinaryPath, GGameIni);
	}
	FString PathToRenderDocDLL = FPaths::Combine(*BinaryPath, *FString("renderdoc.dll"));

	RenderDocDLL = NULL;
	RenderDocDLL = GetModuleHandle(*PathToRenderDocDLL);
	if (BinaryPath.IsEmpty() || !RenderDocDLL)
	{
		UE_LOG(RenderDocPlugin, Error, TEXT("Could not find the renderdoc DLL, have you loaded the RenderDocLoaderPlugin?"));
		return;
	}
	
	// Initialize the RenderDoc API
	pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)GetRenderDocFunctionPointer(RenderDocDLL, "RENDERDOC_GetAPI");
	if (0 == RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_0_0, (void**)&RENDERDOC))
	{
		UE_LOG(RenderDocPlugin, Log, TEXT("RenderDoc initialization failed."));
		return;
	}
	// Version checking
	int major(0), minor(0), patch(0);
	RENDERDOC->GetAPIVersion(&major, &minor, &patch);
	UE_LOG(RenderDocPlugin, Log, TEXT("RenderDoc v%i.%i.%i has been loaded."), major, minor, patch);

	//Set capture settings
	FString RenderDocCapturePath = FPaths::Combine(*FPaths::GameSavedDir(), *FString("RenderDocCaptures"));
	if (!IFileManager::Get().DirectoryExists(*RenderDocCapturePath))
	{
		IFileManager::Get().MakeDirectory(*RenderDocCapturePath, true);
	}

	FString CapturePath = FPaths::Combine(*RenderDocCapturePath, *FDateTime::Now().ToString());
	CapturePath = FPaths::ConvertRelativePathToFull(CapturePath);
	FPaths::NormalizeDirectoryName(CapturePath);
	
	if (sizeof(TCHAR) == sizeof(char))
	{
		RENDERDOC->SetLogFilePathTemplate((const char*)*CapturePath);
	}
	else
	{
		char CapturePathShort[1024];
		ZeroMemory(CapturePathShort, 1024);
		size_t NumCharsConverted;
		wcstombs_s(&NumCharsConverted, CapturePathShort, *CapturePath, CapturePath.Len());
		RENDERDOC->SetLogFilePathTemplate(CapturePathShort);
	}

	RENDERDOC->SetFocusToggleKeys(NULL, 0);
	RENDERDOC->SetCaptureKeys(NULL, 0);

	RENDERDOC->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, RenderDocSettings.bCaptureCallStacks  ? 1 : 0);
	RENDERDOC->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources,   RenderDocSettings.bRefAllResources    ? 1 : 0);
	RENDERDOC->SetCaptureOptionU32(eRENDERDOC_Option_SaveAllInitials,   RenderDocSettings.bSaveAllInitials    ? 1 : 0);

	//Init remote access (no longer necessary?!)
	//RenderDocInitRemoteAccess = (pRENDERDOC_InitRemoteAccess)GetRenderDocFunctionPointer(RenderDocDLL, "RENDERDOC_InitRemoteAccess");
	//SocketPort = 0;
	//RenderDocInitRemoteAccess(&SocketPort);

	//Init UI
	FRenderDocPluginStyle::Initialize();
	FRenderDocPluginCommands::Register();

	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	TSharedRef<FUICommandList> CommandBindings = LevelEditorModule.GetGlobalLevelEditorActions();
	ExtensionManager = LevelEditorModule.GetToolBarExtensibilityManager();

	CommandBindings->MapAction(FRenderDocPluginCommands::Get().CaptureFrame,
		FExecuteAction::CreateRaw(this, &FRenderDocPluginModule::CaptureCurrentViewport),
		FCanExecuteAction());
	CommandBindings->MapAction(FRenderDocPluginCommands::Get().OpenSettings,
		FExecuteAction::CreateRaw(this, &FRenderDocPluginModule::OpenSettingsEditorWindow),
		FCanExecuteAction());

	ToolbarExtender = MakeShareable(new FExtender);
	ToolbarExtension = ToolbarExtender->AddToolBarExtension("CameraSpeed", EExtensionHook::After, CommandBindings,
		FToolBarExtensionDelegate::CreateRaw(this, &FRenderDocPluginModule::AddToolbarExtension));

	LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);

	//Init renderdoc
	RENDERDOC->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);

	RenderDocGUI = new FRenderDocPluginGUI(RENDERDOC->GetCapture);

	IsInitialized = false;
	FSlateRenderer* SlateRenderer = FSlateApplication::Get().GetRenderer().Get();
	LoadedDelegateHandle = SlateRenderer->OnSlateWindowRendered().AddRaw(this, &FRenderDocPluginModule::OnEditorLoaded);

	UE_LOG(RenderDocPlugin, Log, TEXT("RenderDoc plugin is ready!"));
}

void FRenderDocPluginModule::OnEditorLoaded(SWindow& SlateWindow, void* ViewportRHIPtr)
{
	// --> YAGER by SKrysanov 6/11/2014 : fixed crash on removing this callback in render thread.
	if (IsInGameThread())
	{
		FSlateRenderer* SlateRenderer = FSlateApplication::Get().GetRenderer().Get();
		SlateRenderer->OnSlateWindowRendered().Remove(LoadedDelegateHandle);
	}
	// <-- YAGER by SKrysanov 6/11/2014

	if (IsInitialized)
	{
		return;
	}
	IsInitialized = true;

	if (GConfig)
	{
		bool bGreetingHasBeenShown;
		GConfig->GetBool(TEXT("RenderDoc"), TEXT("GreetingHasBeenShown"), bGreetingHasBeenShown, GGameIni);
		if (!bGreetingHasBeenShown)
		{
			GEditor->EditorAddModalWindow(SNew(SRenderDocPluginAboutWindow));
			GConfig->SetBool(TEXT("RenderDoc"), TEXT("GreetingHasBeenShown"), true, GGameIni);
		}
	}

	//TODO: REMOVE THIS WHEN WE GET PULL REQUEST ACCEPTED

	HWND WindowHandle = GetActiveWindow();

	//Trigger a capture just to make sure we are set up correctly. This should prevent us from crashing on exit.
	ENQUEUE_UNIQUE_RENDER_COMMAND_THREEPARAMETER(
		InitializeRenderDoc,
		HWND, WindowHandle, WindowHandle,
		FRenderDocPluginGUI*, RenderDocGUI, RenderDocGUI,
		RENDERDOC_API_CONTEXT*, RENDERDOC, RENDERDOC,
		{
			RENDERDOC_DevicePointer Device = GDynamicRHI->RHIGetNativeDevice();
			RENDERDOC->StartFrameCapture(Device, WindowHandle);
			RENDERDOC->EndFrameCapture(Device, WindowHandle);

			FString NewestCapture = RenderDocGUI->GetNewestCapture(FPaths::Combine(*FPaths::GameSavedDir(), *FString("RenderDocCaptures")));
			IFileManager::Get().Delete(*NewestCapture);
		});

	UE_LOG(RenderDocPlugin, Log, TEXT("RenderDoc plugin initialized!"));

	//TODO: END OF REMOVE THIS
}

void FRenderDocPluginModule::CaptureCurrentViewport()
{
	UE_LOG(RenderDocPlugin, Log, TEXT("Capture frame and launch renderdoc!"));

	FRenderDocPluginNotification::Get().ShowNotification(RenderDocGUI->IsGUIOpen());

	HWND WindowHandle = GetActiveWindow();

	ENQUEUE_UNIQUE_RENDER_COMMAND_THREEPARAMETER(
		StartRenderDocCapture,
		HWND, WindowHandle, WindowHandle,
		RENDERDOC_API_CONTEXT*, RENDERDOC, RENDERDOC,
		FRenderDocPluginModule*, Plugin, this,
		{
			RENDERDOC_DevicePointer Device = GDynamicRHI->RHIGetNativeDevice();
			RENDERDOC->StartFrameCapture(Device, WindowHandle);
		});

	GEditor->GetActiveViewport()->Draw(true);

	ENQUEUE_UNIQUE_RENDER_COMMAND_FIVEPARAMETER(
		EndRenderDocCapture,
		HWND, WindowHandle, WindowHandle,
		uint32, SocketPort, SocketPort,
		FRenderDocPluginGUI*, RenderDocGUI, RenderDocGUI,
		RENDERDOC_API_CONTEXT*, RENDERDOC, RENDERDOC,
		FRenderDocPluginModule*, Plugin, this,
		{
			RENDERDOC_DevicePointer Device = GDynamicRHI->RHIGetNativeDevice();
			RENDERDOC->EndFrameCapture(Device, WindowHandle);

			FString BinaryPath;
			if (GConfig)
			{
			  GConfig->GetString(TEXT("RenderDoc"), TEXT("BinaryPath"), BinaryPath, GGameIni);
			}

			RenderDocGUI->StartRenderDoc(FPaths::Combine(*BinaryPath, *FString("renderdocui.exe"))
			  , FPaths::Combine(*FPaths::GameSavedDir(), *FString("RenderDocCaptures"))
			  , SocketPort);
		});
}

void FRenderDocPluginModule::OpenSettingsEditorWindow()
{
	UE_LOG(RenderDocPlugin, Log, TEXT("Opening settings window"));

	TSharedPtr<SRenderDocPluginSettingsEditorWindow> Window = SNew(SRenderDocPluginSettingsEditorWindow)
		.Settings(RenderDocSettings)
		.SetCaptureOptions(RENDERDOC->SetCaptureOptionU32);

	Window->MoveWindowTo(FSlateApplication::Get().GetCursorPos());
	GEditor->EditorAddModalWindow(Window.ToSharedRef());

	RenderDocSettings = Window->GetSettings();
}

void* FRenderDocPluginModule::GetRenderDocFunctionPointer(HINSTANCE ModuleHandle, LPCSTR FunctionName)
{
	void* OutTarget = NULL;
	OutTarget = (void*)GetProcAddress(ModuleHandle, FunctionName);

	if (!OutTarget)
	{
		UE_LOG(RenderDocPlugin, Error, TEXT("Could not load renderdoc function %s. You are most likely using an incompatible version of Renderdoc"), FunctionName);
	}

	check(OutTarget);
	return OutTarget;
}

void FRenderDocPluginModule::AddToolbarExtension(FToolBarBuilder& ToolbarBuilder)
{
#define LOCTEXT_NAMESPACE "LevelEditorToolBar"

	UE_LOG(RenderDocPlugin, Log, TEXT("Starting extension..."));
	ToolbarBuilder.AddSeparator();

	ToolbarBuilder.BeginSection("RenderdocPlugin");

	FSlateIcon IconBrush = FSlateIcon(FRenderDocPluginStyle::Get()->GetStyleSetName(), "RenderDocPlugin.CaptureFrameIcon.Small");
	ToolbarBuilder.AddToolBarButton(
		FRenderDocPluginCommands::Get().CaptureFrame,
		NAME_None,
		LOCTEXT("RenderDocCapture_Override", "Capture Frame"),
		LOCTEXT("RenderDocCapture_ToolTipOverride", "Captures the next frame and launches the renderdoc UI"),
		IconBrush,
		NAME_None);

	FSlateIcon SettingsIconBrush = FSlateIcon(FRenderDocPluginStyle::Get()->GetStyleSetName(), "RenderDocPlugin.SettingsIcon.Small");
	ToolbarBuilder.AddToolBarButton(
		FRenderDocPluginCommands::Get().OpenSettings,
		NAME_None,
		LOCTEXT("RenderDocCaptureSettings_Override", "Settings"),
		LOCTEXT("RenderDocCaptureSettings_ToolTipOverride", "Edit RenderDoc Settings"),
		SettingsIconBrush,
		NAME_None);

	ToolbarBuilder.EndSection();

#undef LOCTEXT_NAMESPACE
}

void FRenderDocPluginModule::ShutdownModule()
{
	if (GUsingNullRHI)
		return;

	if (ExtensionManager.IsValid())
	{
		FRenderDocPluginStyle::Shutdown();
		FRenderDocPluginCommands::Unregister();

		ToolbarExtender->RemoveExtension(ToolbarExtension.ToSharedRef());

		ExtensionManager->RemoveExtender(ToolbarExtender);
	}
	else
	{
		ExtensionManager.Reset();
	}

	// Unregister the tab spawner
	FGlobalTabmanager::Get()->UnregisterTabSpawner(SettingsUITabName);
}


#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRenderDocPluginModule, RenderDocPlugin)