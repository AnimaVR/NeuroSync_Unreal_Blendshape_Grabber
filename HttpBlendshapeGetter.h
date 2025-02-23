#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "HttpBlendshapeGetter.generated.h"

class UAudioComponent;
class IHttpRequest;
typedef TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> FHttpRequestPtr;

class IHttpResponse;
typedef TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> FHttpResponsePtr;

/** Broadcasts the first-frame array of blendshapes back to Blueprint. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBlendshapesReceived, const TArray<float>&, Blendshapes);

/**
 * UHttpBlendshapeGetter
 *
 * Blueprint node that:
 * 1) Extracts WAV data from a UAudioComponent,
 * 2) Sends it to a Flask endpoint,
 * 3) Parses a 2D array of blendshapes from the response (on a background thread),
 * 4) Removes an all-zero row if found at the start,
 * 5) Saves all frames to CSV in [Project]/Content/gen_anims/ with or without 7 extra columns
 *    (depending on row length 61 or 68),
 * 6) Broadcasts the first frame on the main thread to Blueprint.
 */
UCLASS()
class NEUROSYNC_API UHttpBlendshapeGetter : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/**
	 * Asynchronously get blendshapes from a Flask server endpoint.
	 *
	 * @param AudioComponent	 - The Audio Component containing a valid USoundWave.
	 * @param ServerIP			 - The server IP or hostname (e.g. "127.0.0.1").
	 * @param Port				 - The port number (e.g. 5000).
	 * @param WorldContextObject - Typically "self" in Blueprint.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", DisplayName = "Get Blendshapes from Audio", WorldContext = "WorldContextObject"), Category = "HTTP")
	static UHttpBlendshapeGetter* GetBlendshapesFromAudio(UAudioComponent* AudioComponent, const FString& ServerIP, int32 Port, UObject* WorldContextObject);

	/** Called when blendshapes are successfully received & parsed (for the first frame). */
	UPROPERTY(BlueprintAssignable)
	FOnBlendshapesReceived OnBlendshapesReceived;

	/** Called if the HTTP request or JSON parse fails. */
	UPROPERTY(BlueprintAssignable)
	FOnBlendshapesReceived OnRequestFailed;

private:
	/** Callback once the HTTP request completes. */
	void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	/** Extract raw PCM data from the AudioComponent's SoundWave (editor-only). */
	bool GetAudioBytesFromComponent(UAudioComponent* AudioComponent, TArray<uint8>& OutAudioBytes);

	/**
	 * Saves the entire 2D array to CSV. If the first frame has length = 61, we omit the extra 7 columns.
	 * If it's 68, we include them. Also inserts Timecode and BlendshapeCount columns at the start.
	 */
	void SaveBlendshapes2DToCSV(const TArray<TArray<float>>& In2DBlendshapes) const;

	UPROPERTY()
	UObject* WorldContextObject;
};
