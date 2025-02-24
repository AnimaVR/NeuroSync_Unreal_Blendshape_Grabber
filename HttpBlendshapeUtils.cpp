#pragma once

#include "HttpBlendshapeUtils.h"


#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"

TArray<uint8> CreateWavFileFromPCM(const TArray<uint8>& PCMData, USoundWave* SoundWave)
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
// Utility: Simple timecode at 60 fps
// (Moved from UHttpBlendshapeGetter.cpp)
// ----------------------------------------------------------
FString GenerateTimecodeString(int32 FrameIndex, int32 FPS)
{
    int32 totalSeconds = FrameIndex / FPS;
    int32 frames = FrameIndex % FPS;
    int32 hours = totalSeconds / 3600;
    int32 minutes = (totalSeconds / 60) % 60;
    int32 seconds = totalSeconds % 60;

    return FString::Printf(TEXT("%02d:%02d:%02d:%02d"), hours, minutes, seconds, frames);
}

// ----------------------------------------------------------
// Utility: Saves 2D array to CSV
// (Moved from UHttpBlendshapeGetter.cpp)
// ----------------------------------------------------------
void SaveBlendshapes2DToCSV(const TArray<TArray<float>>& In2DBlendshapes)
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
        TEXT("JawRight"), TEXT("JawLeft"), TEXT("JawOpen"), TEXT("MouthClose"), TEXT("MouthFunnel"),
        TEXT("MouthPucker"), TEXT("MouthRight"), TEXT("MouthLeft"), TEXT("MouthSmileLeft"), TEXT("MouthSmileRight"),
        TEXT("MouthFrownLeft"), TEXT("MouthFrownRight"), TEXT("MouthDimpleLeft"), TEXT("MouthDimpleRight"), TEXT("MouthStretchLeft"),
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

    const int32 BlendshapeCount = Header.Num() - 2; // ignoring Timecode column
    const int32 FPS = 60;

    // Write each row
    for (int32 FrameIndex = 0; FrameIndex < In2DBlendshapes.Num(); FrameIndex++)
    {
        // Timecode column
        CSV.Append(GenerateTimecodeString(FrameIndex, FPS));
        CSV.Append(TEXT(","));

        // BlendshapeCount column
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

    // Write file to [Project]/Content/gen_anims/
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
