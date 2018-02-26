#include "UnrealEnginePythonPrivatePCH.h"

#include "Runtime/MovieSceneCapture/Public/MovieSceneCapture.h"

#if WITH_EDITOR

/*

This is taken as-is (more or less) from MovieSceneCaptureDialogModule.cpp
to automate sequencer capturing

*/

#include "AudioDevice.h"
#include "Editor/EditorEngine.h"
#include "Slate/SceneViewport.h"
#include "AutomatedLevelSequenceCapture.h"

struct FInEditorCapture : TSharedFromThis<FInEditorCapture>
{

	static TWeakPtr<FInEditorCapture> CreateInEditorCapture(UMovieSceneCapture* InCaptureObject)
	{
		// FInEditorCapture owns itself, so should only be kept alive by itself, or a pinned (=> temporary) weakptr
		FInEditorCapture* Capture = new FInEditorCapture;
		Capture->Start(InCaptureObject);
		return Capture->AsShared();
	}

	UWorld* GetWorld() const
	{
		return CapturingFromWorld;
	}

private:
	FInEditorCapture()
	{
		CapturingFromWorld = nullptr;
		CaptureObject = nullptr;
	}

	void Start(UMovieSceneCapture* InCaptureObject)
	{
		check(InCaptureObject);

		CapturingFromWorld = nullptr;
		OnlyStrongReference = MakeShareable(this);

		CaptureObject = InCaptureObject;

		ULevelEditorPlaySettings* PlayInEditorSettings = GetMutableDefault<ULevelEditorPlaySettings>();

		bScreenMessagesWereEnabled = GAreScreenMessagesEnabled;
		GAreScreenMessagesEnabled = false;

		if (!InCaptureObject->Settings.bEnableTextureStreaming)
		{
			const int32 UndefinedTexturePoolSize = -1;
			IConsoleVariable* CVarStreamingPoolSize = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.PoolSize"));
			if (CVarStreamingPoolSize)
			{
				BackedUpStreamingPoolSize = CVarStreamingPoolSize->GetInt();
				CVarStreamingPoolSize->Set(UndefinedTexturePoolSize, ECVF_SetByConsole);
			}

			IConsoleVariable* CVarUseFixedPoolSize = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.UseFixedPoolSize"));
			if (CVarUseFixedPoolSize)
			{
				BackedUpUseFixedPoolSize = CVarUseFixedPoolSize->GetInt();
				CVarUseFixedPoolSize->Set(0, ECVF_SetByConsole);
			}
		}

		FObjectWriter(PlayInEditorSettings, BackedUpPlaySettings);
		OverridePlaySettings(PlayInEditorSettings);

		CaptureObject->AddToRoot();
		CaptureObject->OnCaptureFinished().AddRaw(this, &FInEditorCapture::OnEnd);

		UGameViewportClient::OnViewportCreated().AddRaw(this, &FInEditorCapture::OnStart);
		FEditorDelegates::EndPIE.AddRaw(this, &FInEditorCapture::OnEndPIE);

		FAudioDevice* AudioDevice = GEngine->GetMainAudioDevice();
		if (AudioDevice != nullptr)
		{
			TransientMasterVolume = AudioDevice->GetTransientMasterVolume();
			AudioDevice->SetTransientMasterVolume(0.0f);
		}

		GEditor->RequestPlaySession(true, nullptr, false);
	}

	void OverridePlaySettings(ULevelEditorPlaySettings* PlayInEditorSettings)
	{
		const FMovieSceneCaptureSettings& Settings = CaptureObject->GetSettings();

		PlayInEditorSettings->NewWindowWidth = Settings.Resolution.ResX;
		PlayInEditorSettings->NewWindowHeight = Settings.Resolution.ResY;
		PlayInEditorSettings->CenterNewWindow = true;
		PlayInEditorSettings->LastExecutedPlayModeType = EPlayModeType::PlayMode_InEditorFloating;

		TSharedRef<SWindow> CustomWindow = SNew(SWindow)
			.Title(FText::FromString("Movie Render - Preview"))
			.AutoCenter(EAutoCenter::PrimaryWorkArea)
			.UseOSWindowBorder(true)
			.FocusWhenFirstShown(false)
			.ActivationPolicy(EWindowActivationPolicy::Never)
			.HasCloseButton(true)
			.SupportsMaximize(false)
			.SupportsMinimize(true)
			.MaxWidth(Settings.Resolution.ResX)
			.MaxHeight(Settings.Resolution.ResY)
			.SizingRule(ESizingRule::FixedSize);

		FSlateApplication::Get().AddWindow(CustomWindow);

		PlayInEditorSettings->CustomPIEWindow = CustomWindow;

		// Reset everything else
		PlayInEditorSettings->GameGetsMouseControl = false;
		PlayInEditorSettings->ShowMouseControlLabel = false;
		PlayInEditorSettings->ViewportGetsHMDControl = false;
		PlayInEditorSettings->ShouldMinimizeEditorOnVRPIE = true;
		PlayInEditorSettings->EnableGameSound = false;
		PlayInEditorSettings->bOnlyLoadVisibleLevelsInPIE = false;
		PlayInEditorSettings->bPreferToStreamLevelsInPIE = false;
		PlayInEditorSettings->PIEAlwaysOnTop = false;
		PlayInEditorSettings->DisableStandaloneSound = true;
		PlayInEditorSettings->AdditionalLaunchParameters = TEXT("");
		PlayInEditorSettings->BuildGameBeforeLaunch = EPlayOnBuildMode::PlayOnBuild_Never;
		PlayInEditorSettings->LaunchConfiguration = EPlayOnLaunchConfiguration::LaunchConfig_Default;
		PlayInEditorSettings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
		PlayInEditorSettings->SetRunUnderOneProcess(true);
		PlayInEditorSettings->SetPlayNetDedicated(false);
		PlayInEditorSettings->SetPlayNumberOfClients(1);
	}

	void OnStart()
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE)
			{
				FSlatePlayInEditorInfo* SlatePlayInEditorSession = GEditor->SlatePlayInEditorMap.Find(Context.ContextHandle);
				if (SlatePlayInEditorSession)
				{
					CapturingFromWorld = Context.World();

					TSharedPtr<SWindow> Window = SlatePlayInEditorSession->SlatePlayInEditorWindow.Pin();

					const FMovieSceneCaptureSettings& Settings = CaptureObject->GetSettings();

					SlatePlayInEditorSession->SlatePlayInEditorWindowViewport->SetViewportSize(Settings.Resolution.ResX, Settings.Resolution.ResY);

					FVector2D PreviewWindowSize(Settings.Resolution.ResX, Settings.Resolution.ResY);

					// Keep scaling down the window size while we're bigger than half the destop width/height
					{
						FDisplayMetrics DisplayMetrics;
						FSlateApplication::Get().GetDisplayMetrics(DisplayMetrics);

						while (PreviewWindowSize.X >= DisplayMetrics.PrimaryDisplayWidth*.5f || PreviewWindowSize.Y >= DisplayMetrics.PrimaryDisplayHeight*.5f)
						{
							PreviewWindowSize *= .5f;
						}
					}

					// Resize and move the window into the desktop a bit
					FVector2D PreviewWindowPosition(50, 50);
					Window->ReshapeWindow(PreviewWindowPosition, PreviewWindowSize);

					if (CaptureObject->Settings.GameModeOverride != nullptr)
					{
						CachedGameMode = CapturingFromWorld->GetWorldSettings()->DefaultGameMode;
						CapturingFromWorld->GetWorldSettings()->DefaultGameMode = CaptureObject->Settings.GameModeOverride;
					}

					CaptureObject->Initialize(SlatePlayInEditorSession->SlatePlayInEditorWindowViewport, Context.PIEInstance);
				}
				return;
			}
		}

		// todo: error?
	}

	void Shutdown()
	{
		FEditorDelegates::EndPIE.RemoveAll(this);
		UGameViewportClient::OnViewportCreated().RemoveAll(this);
		CaptureObject->OnCaptureFinished().RemoveAll(this);

		GAreScreenMessagesEnabled = bScreenMessagesWereEnabled;

		if (!CaptureObject->Settings.bEnableTextureStreaming)
		{
			IConsoleVariable* CVarStreamingPoolSize = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.PoolSize"));
			if (CVarStreamingPoolSize)
			{
				CVarStreamingPoolSize->Set(BackedUpStreamingPoolSize, ECVF_SetByConsole);
			}

			IConsoleVariable* CVarUseFixedPoolSize = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.UseFixedPoolSize"));
			if (CVarUseFixedPoolSize)
			{
				CVarUseFixedPoolSize->Set(BackedUpUseFixedPoolSize, ECVF_SetByConsole);
			}
		}

		if (CaptureObject->Settings.GameModeOverride != nullptr)
		{
			CapturingFromWorld->GetWorldSettings()->DefaultGameMode = CachedGameMode;
		}

		FObjectReader(GetMutableDefault<ULevelEditorPlaySettings>(), BackedUpPlaySettings);

		FAudioDevice* AudioDevice = GEngine->GetMainAudioDevice();
		if (AudioDevice != nullptr)
		{
			AudioDevice->SetTransientMasterVolume(TransientMasterVolume);
		}

		CaptureObject->Close();
		CaptureObject->RemoveFromRoot();

	}
	void OnEndPIE(bool bIsSimulating)
	{
		Shutdown();
		OnlyStrongReference = nullptr;
	}

	void OnEnd()
	{
		Shutdown();
		OnlyStrongReference = nullptr;

		GEditor->RequestEndPlayMap();
	}

	TSharedPtr<FInEditorCapture> OnlyStrongReference;
	UWorld* CapturingFromWorld;

	bool bScreenMessagesWereEnabled;
	float TransientMasterVolume;
	int32 BackedUpStreamingPoolSize;
	int32 BackedUpUseFixedPoolSize;
	TArray<uint8> BackedUpPlaySettings;
	UMovieSceneCapture* CaptureObject;

	TSubclassOf<AGameModeBase> CachedGameMode;
};

PyObject *py_ue_in_editor_capture(ue_PyUObject * self, PyObject * args)
{
	ue_py_check(self);

	UMovieSceneCapture *capture = ue_py_check_type<UMovieSceneCapture>(self);
	if (!capture)
		return PyErr_Format(PyExc_Exception, "uobject is not a UMovieSceneCapture");

	FInEditorCapture::CreateInEditorCapture(capture);

	Py_RETURN_NONE;
}

PyObject *py_ue_set_level_sequence_asset(ue_PyUObject *self, PyObject *args)
{
	ue_py_check(self);

	PyObject *py_sequence = nullptr;

	if (!PyArg_ParseTuple(args, "O:set_level_sequence_asset", &py_sequence))
	{
		return nullptr;
	}

	ULevelSequence *sequence = ue_py_check_type<ULevelSequence>(py_sequence);
	if (!sequence)
	{
		return PyErr_Format(PyExc_Exception, "uobject is not a ULevelSequence");
	}

	UAutomatedLevelSequenceCapture *capture = ue_py_check_type<UAutomatedLevelSequenceCapture>(self);
	if (!capture)
		return PyErr_Format(PyExc_Exception, "uobject is not a UAutomatedLevelSequenceCapture");

	capture->SetLevelSequenceAsset(sequence->GetPathName());

	Py_RETURN_NONE;
}
#endif

PyObject *py_ue_capture_initialize(ue_PyUObject * self, PyObject * args)
{

	ue_py_check(self);

	PyObject *py_widget = nullptr;

	if (!PyArg_ParseTuple(args, "|O:capture_initialize", &py_widget))
	{
		return nullptr;
	}

	UMovieSceneCapture *capture = ue_py_check_type<UMovieSceneCapture>(self);
	if (!capture)
		return PyErr_Format(PyExc_Exception, "uobject is not a UMovieSceneCapture");

#if WITH_EDITOR
	if (py_widget)
	{
		ue_PySWidget *s_widget = py_ue_is_swidget(py_widget);
		if (!s_widget)
			return PyErr_Format(PyExc_Exception, "argument is not a SWidget");


		if (s_widget->s_widget->GetType().Compare(FName("SPythonEditorViewport")) == 0)
		{
			TSharedRef<SPythonEditorViewport> s_viewport = StaticCastSharedRef<SPythonEditorViewport>(s_widget->s_widget);
			capture->Initialize(s_viewport->GetSceneViewport());
			capture->StartWarmup();
		}
		else
		{
			return PyErr_Format(PyExc_Exception, "argument is not a supported Viewport-based SWidget");
		}

	}
#endif
	Py_RETURN_NONE;
}

PyObject *py_ue_capture_start(ue_PyUObject * self, PyObject * args)
{

	ue_py_check(self);

	UMovieSceneCapture *capture = ue_py_check_type<UMovieSceneCapture>(self);
	if (!capture)
		return PyErr_Format(PyExc_Exception, "uobject is not a UMovieSceneCapture");

	capture->StartCapture();

	Py_RETURN_NONE;
}

PyObject *py_ue_capture_load_from_config(ue_PyUObject * self, PyObject * args)
{

	ue_py_check(self);

	UMovieSceneCapture *capture = ue_py_check_type<UMovieSceneCapture>(self);
	if (!capture)
		return PyErr_Format(PyExc_Exception, "uobject is not a UMovieSceneCapture");

	capture->LoadFromConfig();

	Py_RETURN_NONE;
}

PyObject *py_ue_capture_stop(ue_PyUObject * self, PyObject * args)
{

	ue_py_check(self);

	UMovieSceneCapture *capture = ue_py_check_type<UMovieSceneCapture>(self);
	if (!capture)
		return PyErr_Format(PyExc_Exception, "uobject is not a UMovieSceneCapture");

	capture->Finalize();
	capture->Close();

	Py_RETURN_NONE;
}
