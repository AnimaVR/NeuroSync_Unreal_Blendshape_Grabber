#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "HttpBlendshapeGetter.generated.h"

class UAudioComponent;
class IHttpRequest;
typedef TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> FHttpRequestPtr;

class IHttpResponse;
typedef TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> FHttpResponsePtr;

/**
 * A single row/frame of blendshapes. This is the "inner array" of floats.
 * Marked as BlueprintType to be usable in Blueprint.
 */
USTRUCT(BlueprintType)
struct FBlendshapeRow
{
    GENERATED_BODY()

    // The actual floats for this single frame
    UPROPERTY(BlueprintReadOnly)
    TArray<float> Data;
};

/**
 * A container for *all* frames of blendshapes, each stored as an FBlendshapeRow.
 */
USTRUCT(BlueprintType)
struct FAllBlendshapesData
{
    GENERATED_BODY()

    // Each element is one row of floats
    UPROPERTY(BlueprintReadOnly)
    TArray<FBlendshapeRow> AllFrames;
};

/** Broadcasts just the *first frame* of floats (the old behavior). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBlendshapesReceived, const TArray<float>&, Blendshapes);

/** Broadcasts *all frames* as an FAllBlendshapesData. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAllBlendshapesReceived, const FAllBlendshapesData&, AllBlendshapes);

/**
 * UHttpBlendshapeGetter
 *
 * 1) Extracts WAV data from a UAudioComponent,
 * 2) Sends it to a Flask endpoint,
 * 3) Parses a 2D array of blendshapes from JSON (on a background thread),
 * 4) Removes an all-zero row if found at the start,
 * 5) Saves all frames to CSV in [Project]/Content/gen_anims/ (61 or 68 columns),
 * 6) Broadcasts the FIRST frame to Blueprint (OnBlendshapesReceived),
 * 7) Also broadcasts ALL frames (OnAllBlendshapesReceived) via FAllBlendshapesData.
 */
UCLASS()
class NEUROSYNC_API UHttpBlendshapeGetter : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    /**
     * Asynchronously get blendshapes from a Flask server endpoint.
     *
     * @param AudioComponent   - The Audio Component with a valid USoundWave.
     * @param ServerIP         - The server IP or hostname (e.g. "127.0.0.1").
     * @param Port             - The port number (e.g. 5000).
     * @param WorldContextObject - Typically "self" in Blueprint.
     */
    UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", DisplayName = "Get Blendshapes from Audio", WorldContext = "WorldContextObject"), Category = "HTTP")
    static UHttpBlendshapeGetter* GetBlendshapesFromAudio(UAudioComponent* AudioComponent, const FString& ServerIP, int32 Port, UObject* WorldContextObject);

    /** Called if the HTTP request or JSON parse fails. */
    UPROPERTY(BlueprintAssignable)
    FOnBlendshapesReceived OnRequestFailed;

    /** Called when blendshapes are successfully parsed (only the *first* frame). */
    UPROPERTY(BlueprintAssignable)
    FOnBlendshapesReceived OnBlendshapesReceived;

    /** Called when *all* frames are successfully parsed (the entire 2D array). */
    UPROPERTY(BlueprintAssignable)
    FOnAllBlendshapesReceived OnAllBlendshapesReceived;

private:
    /** Callback once the HTTP request completes. */
    void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

    /** Extract raw PCM data from the AudioComponent's SoundWave (editor-only). */
    bool GetAudioBytesFromComponent(UAudioComponent* AudioComponent, TArray<uint8>& OutAudioBytes);

    // <<< REMOVED >>> The duplicate SaveBlendshapes2DToCSV declaration is removed
    // void SaveBlendshapes2DToCSV(const TArray<TArray<float>>& In2DBlendshapes) const;

    UPROPERTY()
    UObject* WorldContextObject;
};
