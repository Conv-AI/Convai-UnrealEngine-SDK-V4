// Copyright 2022 Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** Playback-timed cosmetic speech energy. No allocations; caller provides synchronization. */
class FConvaiTalkingPower
{
public:
	void Reset()
	{
		Head = Count = 0;
		Power = 0.0f;
		Reference = MinimumReference;
		HoldRemaining = HeldTarget = 0.0f;
		PendingFrames = PendingRate = PendingChannels = 0;
		PendingEnd = 0.0;
	}

	/** Signed 16-bit interleaved PCM; StartTime is its position in the playback timeline. */
	void Enqueue(const uint8* PCM, uint32 ByteCount, uint32 SampleRate, uint32 Channels, double StartTime)
	{
		if (!PCM || !SampleRate || !Channels || Channels > MaxChannels || Channels > ByteCount / sizeof(int16))
			return;
		if (PendingRate != SampleRate || PendingChannels != Channels || FMath::Abs(StartTime - PendingEnd) > 0.5 / SampleRate)
			PendingFrames = 0;
		PendingRate = SampleRate;
		PendingChannels = Channels;
		const uint32 Frames = ByteCount / sizeof(int16) / Channels;
		const uint32 WindowFrames = FMath::Max(1u, SampleRate / 50u);
		for (uint32 Frame = 0; Frame < Frames;)
		{
			if (!PendingFrames)
			{
				FMemory::Memzero(Sums, sizeof(Sums));
				FMemory::Memzero(Squares, sizeof(Squares));
				PendingStart = StartTime + static_cast<double>(Frame) / SampleRate;
			}
			const uint32 Length = FMath::Min(WindowFrames - PendingFrames, Frames - Frame);
			// Separate channel means reject DC without cancelling opposite-phase stereo speech.
			double Energy = 0.0;
			for (uint32 Channel = 0; Channel < Channels; ++Channel)
			{
				for (uint32 Index = 0; Index < Length; ++Index)
				{
					int16 Sample;
					FMemory::Memcpy(&Sample, PCM + (static_cast<uint64>(Frame + Index) * Channels + Channel) * sizeof(int16), sizeof(Sample));
					const double Value = Sample;
					Sums[Channel] += Value;
					Squares[Channel] += Value * Value;
				}
				const double Mean = Sums[Channel] / (PendingFrames + Length);
				Energy += FMath::Max(0.0, Squares[Channel] / (PendingFrames + Length) - Mean * Mean);
			}
			// Extend a partial window across callbacks, including single-frame chunks.
			// Keep the nearest unplayed windows on overflow; omitted distant audio is fail-quiet.
			const bool bExtend = Count && Windows[(Head + Count - 1) % Capacity].Start == PendingStart;
			if (bExtend || Count < Capacity)
			{
				FWindow& Window = Windows[bExtend ? (Head + Count - 1) % Capacity : (Head + Count++) % Capacity];
				Window.Start = PendingStart;
				Window.End = StartTime + static_cast<double>(Frame + Length) / SampleRate;
				Window.Level = static_cast<float>(FMath::Sqrt(Energy / Channels));
			}
			PendingFrames = (PendingFrames + Length) % WindowFrames;
			Frame += Length;
		}
		PendingEnd = StartTime + static_cast<double>(Frames) / SampleRate;
	}

	float Advance(double PlaybackTime, float DeltaTime, bool bPlaybackActive = true)
	{
		const float Dt = FMath::Max(0.0f, DeltaTime);
		float Level = 0.0f;
		while (bPlaybackActive && Count && Windows[Head].End <= PlaybackTime)
		{
			// Retain short bursts passed by this tick, but never resurrect old speech after a hitch.
			if (Windows[Head].End > PlaybackTime - FMath::Min(Dt, 0.06f))
				Level = FMath::Max(Level, Windows[Head].Level);
			Head = (Head + 1) % Capacity;
			--Count;
		}
		float Target = 0.0f;
		if (bPlaybackActive && Count && PlaybackTime >= Windows[Head].Start)
			Level = FMath::Max(Level, Windows[Head].Level);
		if (Level > NoiseFloor)
		{
			// Adapt only on audible content; silence must never amplify the noise floor.
			Reference = FMath::Max(MinimumReference, FMath::Max(Level, Reference * FMath::Exp(-Dt / 3.0f)));
			Target = FMath::Clamp((Level - NoiseFloor) / (Reference - NoiseFloor), 0.0f, 1.0f);
		}
		if (Target >= HeldTarget)
		{
			HeldTarget = Target;
			HoldRemaining = 0.06f;
		}
		else
		{
			HoldRemaining = FMath::Max(0.0f, HoldRemaining - Dt);
			if (HoldRemaining <= 0.0f)
				HeldTarget = Target;
		}
		const float SmoothedTarget = FMath::Max(Target, HeldTarget);
		const float TimeConstant = SmoothedTarget > Power ? 0.025f : 0.14f;
		Power += (SmoothedTarget - Power) * (1.0f - FMath::Exp(-Dt / TimeConstant));
		if (Power < 0.0001f)
			Power = 0.0f;
		return Power;
	}

	void Silence() { Power = HeldTarget = HoldRemaining = 0.0f; }
	float GetPower() const { return Power; }

private:
	struct FWindow { double Start = 0.0; double End = 0.0; float Level = 0.0f; };
	static constexpr uint32 Capacity = 128;
	static constexpr uint32 MaxChannels = 8;
	static constexpr float NoiseFloor = 32.0f;
	static constexpr float MinimumReference = 64.0f;
	FWindow Windows[Capacity];
	uint32 Head = 0, Count = 0;
	uint32 PendingFrames = 0, PendingRate = 0, PendingChannels = 0;
	double PendingStart = 0.0, PendingEnd = 0.0;
	double Sums[MaxChannels] = {}, Squares[MaxChannels] = {};
	float Power = 0.0f, Reference = MinimumReference, HoldRemaining = 0.0f, HeldTarget = 0.0f;
};
