// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWave.h"

/**
 * Converts raw PCM data into a valid WAV file data buffer.
 * @param PCMData - Raw PCM data.
 * @param SoundWave - The USoundWave instance (used to get sample rate/channels).
 * @return A TArray<uint8> containing the WAV file data.
 */
TArray<uint8> CreateWavFileFromPCM(const TArray<uint8>& PCMData, USoundWave* SoundWave);

/**
 * Generates a timecode string (HH:MM:SS:Frame) based on the frame index and FPS.
 * @param FrameIndex - The index of the frame.
 * @param FPS - Frames per second (default is 60).
 * @return A formatted timecode string.
 */
FString GenerateTimecodeString(int32 FrameIndex, int32 FPS = 60);

/**
 * Saves a 2D array of blendshape data to a CSV file in the project's Content/gen_anims folder.
 * @param In2DBlendshapes - The 2D array of blendshape values.
 */
void SaveBlendshapes2DToCSV(const TArray<TArray<float>>& In2DBlendshapes);
