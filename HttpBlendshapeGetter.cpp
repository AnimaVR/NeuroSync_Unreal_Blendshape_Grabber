#include "HttpBlendshapeGetter.h"
#include "HttpBlendshapeUtils.h"  // <<< ADDED: Use utility functions from this header >>>
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

// <<< REMOVED >>> Duplicate static implementations of CreateWavFileFromPCM, GenerateTimecodeString,
// and the member function SaveBlendshapes2DToCSV have been removed.
// We now rely solely on the versions in HttpBlendshapeUtils.h.

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

    // Convert PCM -> WAV using the utils function
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

            // Save entire data to CSV using the utils function
            SaveBlendshapes2DToCSV(MultiFrameBlendshapes);

            // Wrap the full 2D data for blueprint (see FAllBlendshapesData)
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

                    // Broadcast both the single-frame (legacy) and full blendshape data
                    WeakThis->OnBlendshapesReceived.Broadcast(SingleFrame);
                    WeakThis->OnAllBlendshapesReceived.Broadcast(FullData);
                });
        });
}
