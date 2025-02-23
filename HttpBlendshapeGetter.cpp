#include "HttpBlendshapeGetter.h"

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IHttpRequest.h"
#include "Json.h"
#include "JsonUtilities.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundWave.h"
#include "Async/Future.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Async/Async.h"

// ----------------------------------------------------------
// Utility: Convert raw PCM to WAV
// ----------------------------------------------------------
static TArray<uint8> CreateWavFileFromPCM(const TArray<uint8>& PCMData, USoundWave* SoundWave)
{
    TArray<uint8> WavData;
    int32 DataSize = PCMData.Num();

    int32 SampleRate = SoundWave->GetSampleRateForCurrentPlatform();
    int32 NumChannels = SoundWave->NumChannels;
    int16 BitsPerSample = 16;
    int32 ByteRate = SampleRate * NumChannels * (BitsPerSample / 8);
    int16 BlockAlign = NumChannels * (BitsPerSample / 8);
    int32 ChunkSize = 36 + DataSize;

    WavData.SetNumUninitialized(44 + DataSize);

    FMemory::Memcpy(WavData.GetData(), "RIFF", 4);
    FMemory::Memcpy(WavData.GetData() + 4, &ChunkSize, 4);
    FMemory::Memcpy(WavData.GetData() + 8, "WAVE", 4);

    FMemory::Memcpy(WavData.GetData() + 12, "fmt ", 4);
    int32 SubChunk1Size = 16;
    FMemory::Memcpy(WavData.GetData() + 16, &SubChunk1Size, 4);
    int16 AudioFormat = 1;
    FMemory::Memcpy(WavData.GetData() + 20, &AudioFormat, 2);
    FMemory::Memcpy(WavData.GetData() + 22, &NumChannels, 2);
    FMemory::Memcpy(WavData.GetData() + 24, &SampleRate, 4);
    FMemory::Memcpy(WavData.GetData() + 28, &ByteRate, 4);
    FMemory::Memcpy(WavData.GetData() + 32, &BlockAlign, 2);
    FMemory::Memcpy(WavData.GetData() + 34, &BitsPerSample, 2);

    FMemory::Memcpy(WavData.GetData() + 36, "data", 4);
    FMemory::Memcpy(WavData.GetData() + 40, &DataSize, 4);
    FMemory::Memcpy(WavData.GetData() + 44, PCMData.GetData(), DataSize);

    return WavData;
}

// ----------------------------------------------------------
// Public static function: Create the node & send request
// ----------------------------------------------------------
UHttpBlendshapeGetter* UHttpBlendshapeGetter::GetBlendshapesFromAudio(
    UAudioComponent* AudioComponent,
    const FString& ServerIP,
    int32 Port,
    UObject* WorldContextObject)
{
    UHttpBlendshapeGetter* Node = NewObject<UHttpBlendshapeGetter>();
    Node->WorldContextObject = WorldContextObject;

    if (!AudioComponent)
    {
        UE_LOG(LogTemp, Warning, TEXT("GetBlendshapesFromAudio: Null AudioComponent."));
        Node->OnRequestFailed.Broadcast(TArray<float>());
        return Node;
    }

    USoundWave* SoundWave = Cast<USoundWave>(AudioComponent->Sound);
    if (!SoundWave)
    {
        UE_LOG(LogTemp, Warning, TEXT("GetBlendshapesFromAudio: No valid SoundWave."));
        Node->OnRequestFailed.Broadcast(TArray<float>());
        return Node;
    }

    // Extract PCM
    TArray<uint8> PCMData;
    if (!Node->GetAudioBytesFromComponent(AudioComponent, PCMData))
    {
        // OnRequestFailed was likely broadcast
        return Node;
    }

    // Convert PCM -> WAV
    TArray<uint8> WAVData = CreateWavFileFromPCM(PCMData, SoundWave);

    // Build URL
    FString URL = FString::Printf(TEXT("http://%s:%d/audio_to_blendshapes"), *ServerIP, Port);

    // Create HTTP request
    FHttpModule* Http = &FHttpModule::Get();
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http->CreateRequest();
    Request->SetVerb("POST");
    Request->SetURL(URL);
    Request->SetHeader("Content-Type", "application/octet-stream");
    Request->SetContent(WAVData);

    // Bind callback
    Request->OnProcessRequestComplete().BindUObject(Node, &UHttpBlendshapeGetter::OnResponseReceived);
    Request->ProcessRequest();

    return Node;
}

// ----------------------------------------------------------
// Extract raw PCM from AudioComponent (editor-only)
// ----------------------------------------------------------
bool UHttpBlendshapeGetter::GetAudioBytesFromComponent(UAudioComponent* AudioComponent, TArray<uint8>& OutAudioBytes)
{
    if (!AudioComponent)
    {
        UE_LOG(LogTemp, Warning, TEXT("GetAudioBytesFromComponent: Null AudioComponent."));
        return false;
    }

    USoundWave* SoundWave = Cast<USoundWave>(AudioComponent->Sound);
    if (!SoundWave)
    {
        UE_LOG(LogTemp, Warning, TEXT("GetAudioBytesFromComponent: No valid SoundWave."));
        return false;
    }

#if WITH_EDITOR
    TFuture<FSharedBuffer> PayloadFuture = SoundWave->RawData.GetPayload();
    FSharedBuffer PayloadBuffer = PayloadFuture.Get(); // blocking
    int32 DataSize = PayloadBuffer.GetSize();
    if (DataSize > 0)
    {
        OutAudioBytes.SetNumUninitialized(DataSize);
        FMemory::Memcpy(OutAudioBytes.GetData(), PayloadBuffer.GetData(), DataSize);
        return true;
    }
#else
    UE_LOG(LogTemp, Warning, TEXT("GetAudioBytesFromComponent: Not supported at runtime."));
    return false;
#endif

    UE_LOG(LogTemp, Warning, TEXT("GetAudioBytesFromComponent: No raw data in SoundWave."));
    return false;
}

// ----------------------------------------------------------
// HTTP callback -> parse in background thread -> broadcast
// ----------------------------------------------------------
void UHttpBlendshapeGetter::OnResponseReceived(
    FHttpRequestPtr Request,
    FHttpResponsePtr Response,
    bool bWasSuccessful)
{
    if (!bWasSuccessful || !Response.IsValid())
    {
        OnRequestFailed.Broadcast(TArray<float>());
        return;
    }

    // Get the JSON as string
    const FString ResponseStr = Response->GetContentAsString();

    TWeakObjectPtr<UHttpBlendshapeGetter> WeakThis(this);
    // Parse JSON & save CSV off the game thread
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, ResponseStr]()
        {
            if (!WeakThis.IsValid())
            {
                return;
            }

            // Parse JSON
            TSharedPtr<FJsonObject> JsonObject;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseStr);
            if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
            {
                // Fail on main thread
                AsyncTask(ENamedThreads::GameThread, [WeakThis]()
                    {
                        if (WeakThis.IsValid())
                        {
                            WeakThis->OnRequestFailed.Broadcast(TArray<float>());
                        }
                    });
                return;
            }

            if (!JsonObject->HasTypedField<EJson::Array>(TEXT("blendshapes")))
            {
                // Fail
                AsyncTask(ENamedThreads::GameThread, [WeakThis]()
                    {
                        if (WeakThis.IsValid())
                        {
                            WeakThis->OnRequestFailed.Broadcast(TArray<float>());
                        }
                    });
                return;
            }

            const TArray<TSharedPtr<FJsonValue>>* OuterArray = nullptr;
            if (!JsonObject->TryGetArrayField(TEXT("blendshapes"), OuterArray))
            {
                // Fail
                AsyncTask(ENamedThreads::GameThread, [WeakThis]()
                    {
                        if (WeakThis.IsValid())
                        {
                            WeakThis->OnRequestFailed.Broadcast(TArray<float>());
                        }
                    });
                return;
            }

            // Build a 2D array: TArray<TArray<float>>
            TArray<TArray<float>> MultiFrameBlendshapes;
            for (const TSharedPtr<FJsonValue>& OuterVal : *OuterArray)
            {
                if (!OuterVal.IsValid() || OuterVal->Type != EJson::Array)
                {
                    continue;
                }
                const TArray<TSharedPtr<FJsonValue>>& InnerArr = OuterVal->AsArray();

                TArray<float> OneFrame;
                for (const TSharedPtr<FJsonValue>& InnerVal : InnerArr)
                {
                    OneFrame.Add(InnerVal->AsNumber());
                }
                MultiFrameBlendshapes.Add(OneFrame);
            }

            // Optionally remove an all-zero first row
            if (MultiFrameBlendshapes.Num() > 0)
            {
                bool bAllZero = true;
                for (float Val : MultiFrameBlendshapes[0])
                {
                    if (Val != 0.0f)
                    {
                        bAllZero = false;
                        break;
                    }
                }
                if (bAllZero)
                {
                    MultiFrameBlendshapes.RemoveAt(0);
                }
            }

            // Single frame for blueprint
            TArray<float> SingleFrame;
            if (MultiFrameBlendshapes.Num() > 0)
            {
                SingleFrame = MultiFrameBlendshapes[0];
            }

            // Save entire data to CSV
            WeakThis->SaveBlendshapes2DToCSV(MultiFrameBlendshapes);

            // We now want to broadcast *all frames* as well. 
            // But we can't do TArray<TArray<float>> in a UPROPERTY.
            // So we wrap it in FAllBlendshapesData => TArray<FBlendshapeRow>.
            FAllBlendshapesData FullData;
            for (const TArray<float>& Frame : MultiFrameBlendshapes)
            {
                FBlendshapeRow Row;
                Row.Data = Frame; // copy
                FullData.AllFrames.Add(Row);
            }

            // Now broadcast on main thread
            AsyncTask(ENamedThreads::GameThread, [WeakThis, SingleFrame, FullData]()
                {
                    if (!WeakThis.IsValid())
                    {
                        return;
                    }

                    // The user said they want the single frame (original behavior) 
                    // *and* the full data. We'll keep the single-frame broadcast 
                    // to not break old functionality.
                    WeakThis->OnBlendshapesReceived.Broadcast(SingleFrame);
                    WeakThis->OnAllBlendshapesReceived.Broadcast(FullData);
                });
        });
}

// ----------------------------------------------------------
// Simple timecode at 60 fps
// ----------------------------------------------------------
static FString GenerateTimecodeString(int32 FrameIndex, int32 FPS = 60)
{
    int32 totalSeconds = FrameIndex / FPS;
    int32 frames = FrameIndex % FPS;
    int32 hours = totalSeconds / 3600;
    int32 minutes = (totalSeconds / 60) % 60;
    int32 seconds = totalSeconds % 60;

    return FString::Printf(TEXT("%02d:%02d:%02d:%02d"), hours, minutes, seconds, frames);
}

// ----------------------------------------------------------
// Saves 2D array to CSV (61 or 68 columns + 2 columns for timecode/blendshape count)
// ----------------------------------------------------------
void UHttpBlendshapeGetter::SaveBlendshapes2DToCSV(const TArray<TArray<float>>& In2DBlendshapes) const
{
    if (In2DBlendshapes.Num() == 0)
    {
        return; // no frames => no CSV
    }

    const int32 FirstRowLength = In2DBlendshapes[0].Num();

    static const TArray<FString> BaseLabels = {
        TEXT("EyeBlinkLeft"), TEXT("EyeLookDownLeft"), TEXT("EyeLookInLeft"), TEXT("EyeLookOutLeft"), TEXT("EyeLookUpLeft"),
        TEXT("EyeSquintLeft"), TEXT("EyeWideLeft"), TEXT("EyeBlinkRight"), TEXT("EyeLookDownRight"), TEXT("EyeLookInRight"),
        TEXT("EyeLookOutRight"), TEXT("EyeLookUpRight"), TEXT("EyeSquintRight"), TEXT("EyeWideRight"), TEXT("JawForward"),
        TEXT("JawRight"), TEXT("JawLeft"), TEXT("JawOpen"), TEXT("MouthClose"), TEXT("MouthFunnel"), TEXT("MouthPucker"),
        TEXT("MouthRight"), TEXT("MouthLeft"), TEXT("MouthSmileLeft"), TEXT("MouthSmileRight"), TEXT("MouthFrownLeft"),
        TEXT("MouthFrownRight"), TEXT("MouthDimpleLeft"), TEXT("MouthDimpleRight"), TEXT("MouthStretchLeft"),
        TEXT("MouthStretchRight"), TEXT("MouthRollLower"), TEXT("MouthRollUpper"), TEXT("MouthShrugLower"),
        TEXT("MouthShrugUpper"), TEXT("MouthPressLeft"), TEXT("MouthPressRight"), TEXT("MouthLowerDownLeft"),
        TEXT("MouthLowerDownRight"), TEXT("MouthUpperUpLeft"), TEXT("MouthUpperUpRight"), TEXT("BrowDownLeft"),
        TEXT("BrowDownRight"), TEXT("BrowInnerUp"), TEXT("BrowOuterUpLeft"), TEXT("BrowOuterUpRight"), TEXT("CheekPuff"),
        TEXT("CheekSquintLeft"), TEXT("CheekSquintRight"), TEXT("NoseSneerLeft"), TEXT("NoseSneerRight"), TEXT("TongueOut"),
        TEXT("HeadYaw"), TEXT("HeadPitch"), TEXT("HeadRoll"), TEXT("LeftEyeYaw"), TEXT("LeftEyePitch"), TEXT("LeftEyeRoll"),
        TEXT("RightEyeYaw"), TEXT("RightEyePitch"), TEXT("RightEyeRoll")
    };

    static const TArray<FString> ExtraLabels = {
        TEXT("1"), TEXT("2"), TEXT("3"), TEXT("4"), TEXT("5"), TEXT("6"), TEXT("7")
    };

    bool bHasExtra = (FirstRowLength == 68);

    // Build header
    TArray<FString> Header;
    Header.Add(TEXT("Timecode"));
    Header.Add(TEXT("BlendshapeCount"));

    for (const FString& Base : BaseLabels)
    {
        Header.Add(Base);
    }
    if (bHasExtra)
    {
        for (const FString& Extra : ExtraLabels)
        {
            Header.Add(Extra);
        }
    }

    FString CSV;
    // Header row
    for (int32 i = 0; i < Header.Num(); i++)
    {
        CSV.Append(Header[i]);
        if (i < Header.Num() - 1)
        {
            CSV.Append(TEXT(","));
        }
    }
    CSV.Append(TEXT("\n"));

    const int32 BlendshapeCount = Header.Num() - 2; // ignoring Timecode
    const int32 FPS = 60;

    // Rows
    for (int32 FrameIndex = 0; FrameIndex < In2DBlendshapes.Num(); FrameIndex++)
    {
        // Timecode
        CSV.Append(GenerateTimecodeString(FrameIndex, FPS));
        CSV.Append(TEXT(","));

        // Count
        CSV.Append(FString::FromInt(BlendshapeCount));
        CSV.Append(TEXT(","));

        const TArray<float>& Row = In2DBlendshapes[FrameIndex];
        for (int32 c = 0; c < Row.Num(); c++)
        {
            CSV.Append(FString::SanitizeFloat(Row[c]));
            if (c < Row.Num() - 1)
            {
                CSV.Append(TEXT(","));
            }
        }
        CSV.Append(TEXT("\n"));
    }

    // Write file
    FString Directory = FPaths::ProjectContentDir() / TEXT("gen_anims");
    FString FileName = FString::Printf(TEXT("blendshapes_%s.csv"), *FGuid::NewGuid().ToString());
    FString FullPath = Directory / FileName;

    IFileManager& FileManager = IFileManager::Get();
    if (!FileManager.DirectoryExists(*Directory))
    {
        FileManager.MakeDirectory(*Directory);
    }
    FFileHelper::SaveStringToFile(CSV, *FullPath);
}
