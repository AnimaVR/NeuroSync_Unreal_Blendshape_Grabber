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

///////////////////////////////////////////////////////////////////////////////////////
// 1) Utility: Convert PCM to WAV
///////////////////////////////////////////////////////////////////////////////////////
static TArray<uint8> CreateWavFileFromPCM(const TArray<uint8>& PCMData, USoundWave* SoundWave)
{
    TArray<uint8> WavData;
    const int32 DataSize = PCMData.Num();

    const int32 SampleRate = SoundWave->GetSampleRateForCurrentPlatform();
    const int32 NumChannels = SoundWave->NumChannels;
    const int16 BitsPerSample = 16;
    const int32 ByteRate = SampleRate * NumChannels * (BitsPerSample / 8);
    const int16 BlockAlign = NumChannels * (BitsPerSample / 8);
    const int32 ChunkSize = 36 + DataSize;

    WavData.SetNumUninitialized(44 + DataSize);

    FMemory::Memcpy(WavData.GetData(), "RIFF", 4);
    FMemory::Memcpy(WavData.GetData() + 4, &ChunkSize, 4);
    FMemory::Memcpy(WavData.GetData() + 8, "WAVE", 4);

    FMemory::Memcpy(WavData.GetData() + 12, "fmt ", 4);
    const int32 SubChunk1Size = 16;
    FMemory::Memcpy(WavData.GetData() + 16, &SubChunk1Size, 4);
    const int16 AudioFormat = 1; // PCM
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

///////////////////////////////////////////////////////////////////////////////////////
// 2) Public entry: Create our async node, send the request
///////////////////////////////////////////////////////////////////////////////////////
UHttpBlendshapeGetter* UHttpBlendshapeGetter::GetBlendshapesFromAudio(UAudioComponent* AudioComponent, const FString& ServerIP, int32 Port, UObject* WorldContextObject)
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
        // OnRequestFailed likely already broadcast
        return Node;
    }

    // Convert to WAV
    TArray<uint8> WAVData = CreateWavFileFromPCM(PCMData, SoundWave);

    // Build the URL
    const FString URL = FString::Printf(TEXT("http://%s:%d/audio_to_blendshapes"), *ServerIP, Port);

    // Make the HTTP request
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

///////////////////////////////////////////////////////////////////////////////////////
// 3) Extract PCM from SoundWave (editor-only)
///////////////////////////////////////////////////////////////////////////////////////
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
    const int32 DataSize = PayloadBuffer.GetSize();
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

///////////////////////////////////////////////////////////////////////////////////////
// 4) The HTTP callback: spawn a background thread to parse and save
///////////////////////////////////////////////////////////////////////////////////////
void UHttpBlendshapeGetter::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    if (!bWasSuccessful || !Response.IsValid())
    {
        OnRequestFailed.Broadcast(TArray<float>());
        return;
    }

    // Capture string locally
    const FString ResponseStr = Response->GetContentAsString();

    TWeakObjectPtr<UHttpBlendshapeGetter> WeakThis(this);
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
                // Fail on game thread
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

            // 2D array
            TArray<TArray<float>> MultiFrameBlendshapes;
            for (const TSharedPtr<FJsonValue>& OuterVal : *OuterArray)
            {
                if (!OuterVal.IsValid() || OuterVal->Type != EJson::Array)
                {
                    continue;
                }
                const TArray<TSharedPtr<FJsonValue>>& InnerArray = OuterVal->AsArray();

                TArray<float> OneFrame;
                for (const TSharedPtr<FJsonValue>& InnerVal : InnerArray)
                {
                    OneFrame.Add(InnerVal->AsNumber());
                }
                MultiFrameBlendshapes.Add(OneFrame);
            }

            // Remove all-zero row if first is zero
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

            // The "first frame" for blueprint
            TArray<float> SingleFrame;
            if (MultiFrameBlendshapes.Num() > 0)
            {
                SingleFrame = MultiFrameBlendshapes[0];
            }

            // Save entire data
            WeakThis->SaveBlendshapes2DToCSV(MultiFrameBlendshapes);

            // Jump back to game thread for success
            AsyncTask(ENamedThreads::GameThread, [WeakThis, SingleFrame]()
                {
                    if (WeakThis.IsValid())
                    {
                        WeakThis->OnBlendshapesReceived.Broadcast(SingleFrame);
                    }
                });
        });
}

///////////////////////////////////////////////////////////////////////////////////////
// 5) Timecode utility
///////////////////////////////////////////////////////////////////////////////////////
static FString GenerateTimecodeString(int32 FrameIndex, int32 FPS = 60)
{
    const int32 totalSeconds = FrameIndex / FPS;
    const int32 frames = FrameIndex % FPS;
    const int32 hours = totalSeconds / 3600;
    const int32 minutes = (totalSeconds / 60) % 60;
    const int32 seconds = totalSeconds % 60;

    return FString::Printf(TEXT("%02d:%02d:%02d:%02d"), hours, minutes, seconds, frames);
}

///////////////////////////////////////////////////////////////////////////////////////
// 6) Save CSV with or without extra columns (detected by first row length)
///////////////////////////////////////////////////////////////////////////////////////
void UHttpBlendshapeGetter::SaveBlendshapes2DToCSV(const TArray<TArray<float>>& In2DBlendshapes) const
{
    if (In2DBlendshapes.Num() == 0)
    {
        // No frames => no CSV needed
        return;
    }

    // Check the first frame's length
    const int32 FirstRowLength = In2DBlendshapes[0].Num();

    // Suppose your "base" set of columns is 61. If length==68, we add 7 extra columns.
    // We'll dynamically build the header array.

    // The "base" 61 column labels (not counting Timecode, BlendshapeCount).
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

    // The 7 extra columns
    static const TArray<FString> ExtraLabels = {
        TEXT("1"), TEXT("2"), TEXT("3"), TEXT("4"), TEXT("5"), TEXT("6"), TEXT("7")
    };

    // Are we 61 or 68?
    bool bHasExtraColumns = (FirstRowLength == 68);

    // Build the final header list: 2 columns for Timecode,BlendshapeCount + base + optional extras
    TArray<FString> HeaderLabels;
    HeaderLabels.Add(TEXT("Timecode"));
    HeaderLabels.Add(TEXT("BlendshapeCount"));

    for (const FString& Base : BaseLabels)
    {
        HeaderLabels.Add(Base);
    }

    if (bHasExtraColumns)
    {
        // The user wants 7 extra columns if the row is 68
        for (const FString& Extra : ExtraLabels)
        {
            HeaderLabels.Add(Extra);
        }
    }

    // Now let's produce the CSV
    FString CSV;
    // 1) Header row
    for (int32 i = 0; i < HeaderLabels.Num(); i++)
    {
        CSV.Append(HeaderLabels[i]);
        if (i < HeaderLabels.Num() - 1)
        {
            CSV.Append(TEXT(","));
        }
    }
    CSV.Append(TEXT("\n"));

    // 2) We'll set "BlendshapeCount" to the total # of columns after Timecode
    //    i.e. either 61 or 68
    const int32 ColumnsAfterTimecode = HeaderLabels.Num() - 1; // minus the "Timecode" itself
    // (Alternatively, you can just do "FirstRowLength" for the count, etc.)

    // 3) For each frame, Timecode + blendshapeCount + the actual floats
    const int32 FPS = 60;
    for (int32 FrameIndex = 0; FrameIndex < In2DBlendshapes.Num(); FrameIndex++)
    {
        // Timecode
        CSV.Append(GenerateTimecodeString(FrameIndex, FPS));
        CSV.Append(TEXT(","));

        // BlendshapeCount
        CSV.Append(FString::FromInt(ColumnsAfterTimecode));
        CSV.Append(TEXT(","));

        // The row's floats
        const TArray<float>& RowData = In2DBlendshapes[FrameIndex];
        for (int32 i = 0; i < RowData.Num(); i++)
        {
            CSV.Append(FString::SanitizeFloat(RowData[i]));
            if (i < RowData.Num() - 1)
            {
                CSV.Append(TEXT(","));
            }
        }
        CSV.Append(TEXT("\n"));
    }

    // Finally, write to disk
    const FString Directory = FPaths::ProjectContentDir() / TEXT("gen_anims");
    const FString FileName = FString::Printf(TEXT("blendshapes_%s.csv"), *FGuid::NewGuid().ToString());
    const FString FullPath = Directory / FileName;

    IFileManager& FileManager = IFileManager::Get();
    if (!FileManager.DirectoryExists(*Directory))
    {
        FileManager.MakeDirectory(*Directory);
    }

    FFileHelper::SaveStringToFile(CSV, *FullPath);
}
