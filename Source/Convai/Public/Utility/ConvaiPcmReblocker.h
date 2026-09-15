// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Utility/ConvaiEngineCompat.h"

/**
 * Turns a submix render callback into the fixed 10 ms mono chunks the echo
 * canceller accepts, without losing a sample between callbacks.
 *
 * Every producer of audio in the engine is clocked by something other than the
 * canceller: a mixer callback delivers whatever its buffer size is, a device
 * delivers whatever its period is. Converting each callback independently — a
 * fresh resampler phase, a dropped tail, one channel of several — costs a few
 * samples per callback, and a few samples per callback is the near and far
 * streams walking apart at milliseconds per second. A canceller can model a
 * fixed delay; it cannot model a delay that grows for the length of a
 * conversation. So every piece of state that spans callbacks lives here:
 * the fractional resampler phase, the last input frame, and the partial chunk.
 *
 * Not thread-safe. Each instance belongs to one producer.
 */
class FConvaiPcmReblocker
{
public:
	FConvaiPcmReblocker(int32 InTargetSampleRate, int32 InChunkSamples)
		: TargetSampleRate(FMath::Max(1, InTargetSampleRate))
		, ChunkSamples(FMath::Max(1, InChunkSamples))
	{
	}

	/** Interleaved float from a submix callback. NumSamples counts every channel. */
	void PushInterleavedFloat(const float* Samples, int32 NumSamples, int32 NumChannels, int32 SampleRate)
	{
		if (!Samples || NumSamples <= 0 || NumChannels <= 0 || SampleRate <= 0)
		{
			return;
		}

		// A format change invalidates the interpolation history; carrying it
		// across would splice two different timelines together.
		if (SampleRate != SourceSampleRate || NumChannels != SourceChannels)
		{
			SourceSampleRate = SampleRate;
			SourceChannels = NumChannels;
			Phase = 0.0;
			PrevSample = 0.0f;
			bPrimed = false;
		}

		const int32 NumFrames = NumSamples / NumChannels;
		if (NumFrames <= 0)
		{
			return;
		}

		// Downmix by averaging. Taking channel 0 instead would under-represent
		// anything panned away from it — and the far end is exactly the signal
		// the canceller has to match.
		Mono.SetNumUninitialized(NumFrames, CONVAI_ALLOW_SHRINKING_NO);
		if (NumChannels == 1)
		{
			FMemory::Memcpy(Mono.GetData(), Samples, NumFrames * sizeof(float));
		}
		else
		{
			const float Scale = 1.0f / static_cast<float>(NumChannels);
			for (int32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				float Sum = 0.0f;
				const float* Base = Samples + Frame * NumChannels;
				for (int32 Ch = 0; Ch < NumChannels; ++Ch)
				{
					Sum += Base[Ch];
				}
				Mono[Frame] = Sum * Scale;
			}
		}

		if (SourceSampleRate == TargetSampleRate)
		{
			for (int32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				Pending.Add(ToPcm16(Mono[Frame]));
			}
			PrevSample = Mono[NumFrames - 1];
			bPrimed = true;
			return;
		}

		// Linear interpolation over a history that starts at the previous
		// callback's last frame, so position 0 is continuous with what came
		// before. Phase is kept in input-frame units and carried across calls.
		// ponytail: no anti-alias filter. The engine renders at 44.1 or 48 kHz
		// against a 48 kHz target, so this is upsampling or unity; add a
		// low-pass here if a genuinely higher-rate device ever appears.
		const double Ratio = static_cast<double>(SourceSampleRate) / static_cast<double>(TargetSampleRate);
		if (!bPrimed)
		{
			PrevSample = Mono[0];
			bPrimed = true;
		}

		while (Phase < static_cast<double>(NumFrames))
		{
			const int32 Index = FMath::FloorToInt(Phase);
			const float Frac = static_cast<float>(Phase - static_cast<double>(Index));
			const float A = (Index == 0) ? PrevSample : Mono[Index - 1];
			const float B = Mono[Index];
			Pending.Add(ToPcm16(FMath::Lerp(A, B, Frac)));
			Phase += Ratio;
		}
		Phase -= static_cast<double>(NumFrames);
		PrevSample = Mono[NumFrames - 1];
	}

	/** Invokes Fn(const int16*, int32) per whole chunk; the remainder is carried. */
	template <typename FnT>
	int32 Drain(FnT&& Fn)
	{
		int32 Emitted = 0;
		while (Pending.Num() - Emitted * ChunkSamples >= ChunkSamples)
		{
			Fn(Pending.GetData() + Emitted * ChunkSamples, ChunkSamples);
			++Emitted;
		}
		if (Emitted > 0)
		{
			Pending.RemoveAt(0, Emitted * ChunkSamples, CONVAI_ALLOW_SHRINKING_NO);
		}
		return Emitted;
	}

	void Reset()
	{
		Pending.Reset();
		Mono.Reset();
		Phase = 0.0;
		PrevSample = 0.0f;
		bPrimed = false;
		SourceSampleRate = 0;
		SourceChannels = 0;
	}

	int32 Carry() const { return Pending.Num(); }
	int32 GetChunkSamples() const { return ChunkSamples; }
	int32 GetSourceSampleRate() const { return SourceSampleRate; }
	int32 GetSourceChannels() const { return SourceChannels; }

private:
	static int16 ToPcm16(float Value)
	{
		return static_cast<int16>(FMath::Clamp(FMath::RoundToInt(Value * 32767.0f), -32768, 32767));
	}

	int32 TargetSampleRate;
	int32 ChunkSamples;

	int32 SourceSampleRate = 0;
	int32 SourceChannels = 0;

	double Phase = 0.0;
	float PrevSample = 0.0f;
	bool bPrimed = false;

	TArray<float> Mono;
	TArray<int16> Pending;
};
