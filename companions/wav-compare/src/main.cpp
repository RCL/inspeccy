// Spectral and envelope comparison of two PCM WAV files. A companion to
// inspeccy rather than part of it: it reads only files, knows nothing about a
// Spectrum, and links nothing.
//
// Both inputs are downmixed to mono and linearly resampled to 11025 Hz. The
// spectrum and envelope scores tolerate some phase difference; raw waveform
// correlation is reported separately. These are heuristic metrics, not proof
// of musical equivalence or matching clocks: stereo differences are lost and
// linear resampling can alias high-frequency content.
//
// Exit codes: 0 when the configured thresholds pass, 1 when they fail, 2 on
// invalid input. The drift estimate is diagnostic and does not affect them.
// 8-, 16- and 32-bit integer PCM are supported.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <complex>
#include <string>
#include <vector>

#ifndef M_PI	// MSVC's <math.h> defines it only under _USE_MATH_DEFINES
#define M_PI 3.14159265358979323846
#endif

namespace
{

constexpr int kAnalysisRate = 11025; // analysis bandwidth is about 5.5 kHz
constexpr int kFftN = 1024;			 // ~93 ms frames at 11025 Hz
constexpr int kFftHop = 512;
constexpr int kEnvBlock = 128; // envelope rate = 11025/128 ~ 86 Hz
constexpr int kBands = 64;

int Fail(const std::string& Message)
{
	fprintf(stderr, "wav-compare: %s\n", Message.c_str());
	return 2;
}

// ------------------------------------------------------------------- WAV
// Just enough RIFF for integer PCM. A data chunk that claims more bytes than
// the file holds is a truncated recording, not a short one, so it is refused.
bool ReadWavMono(const std::string& Path, std::vector<double>& Out, uint32_t& OutRate,
	std::string& OutError)
{
	FILE* F = fopen(Path.c_str(), "rb");
	if (F == nullptr)
	{
		OutError = "cannot open " + Path;
		return false;
	}
	std::vector<uint8_t> Bytes;
	uint8_t Chunk[65536];
	size_t Got = 0;
	while ((Got = fread(Chunk, 1, sizeof(Chunk), F)) > 0)
	{
		Bytes.insert(Bytes.end(), Chunk, Chunk + Got);
		if (Bytes.size() > (512u << 20))
		{
			break;
		}
	}
	fclose(F);

	auto U32 = [&](size_t At)
	{
		return uint32_t(Bytes[At]) | (uint32_t(Bytes[At + 1]) << 8)
			| (uint32_t(Bytes[At + 2]) << 16) | (uint32_t(Bytes[At + 3]) << 24);
	};
	auto U16 = [&](size_t At)
	{
		return uint16_t(uint16_t(Bytes[At]) | (uint16_t(Bytes[At + 1]) << 8));
	};
	if (Bytes.size() < 44 || memcmp(&Bytes[0], "RIFF", 4) != 0
		|| memcmp(&Bytes[8], "WAVE", 4) != 0)
	{
		OutError = Path + ": not a RIFF/WAVE file";
		return false;
	}

	uint16_t Channels = 0, Width = 0, Format = 0;
	size_t DataAt = 0, DataSize = 0;
	bool bHaveFmt = false, bHaveData = false;
	for (size_t At = 12; At + 8 <= Bytes.size();)
	{
		const uint32_t Size = U32(At + 4);
		const size_t Body = At + 8;
		if (memcmp(&Bytes[At], "fmt ", 4) == 0 && Size >= 16 && Body + 16 <= Bytes.size())
		{
			Format = U16(Body);
			Channels = U16(Body + 2);
			OutRate = U32(Body + 4);
			Width = uint16_t(U16(Body + 14) / 8);
			bHaveFmt = true;
		}
		else if (memcmp(&Bytes[At], "data", 4) == 0)
		{
			DataAt = Body;
			DataSize = Size;
			bHaveData = true;
			break;
		}
		At = Body + Size + (Size & 1);
	}
	if (!bHaveFmt || !bHaveData)
	{
		OutError = Path + ": missing fmt or data chunk";
		return false;
	}
	if (Format != 1)
	{
		OutError = Path + ": only integer PCM is supported";
		return false;
	}
	if (Channels == 0 || (Width != 1 && Width != 2 && Width != 4))
	{
		OutError = Path + ": unsupported sample width " + std::to_string(Width);
		return false;
	}
	if (DataAt + DataSize > Bytes.size())
	{
		OutError = Path + ": truncated PCM data";
		return false;
	}

	const size_t Frame = size_t(Channels) * Width;
	const size_t Frames = DataSize / Frame;
	Out.assign(Frames, 0.0);
	const double Inv = 1.0 / double(Channels);
	for (size_t IdxI = 0; IdxI < Frames; ++IdxI)
	{
		double Sum = 0.0;
		for (uint16_t C = 0; C < Channels; ++C)
		{
			const uint8_t* P = &Bytes[DataAt + IdxI * Frame + size_t(C) * Width];
			if (Width == 1)
			{
				Sum += (double(P[0]) - 128.0) / 128.0; // 8-bit WAV is unsigned
			}
			else if (Width == 2)
			{
				Sum += double(int16_t(uint16_t(P[0]) | (uint16_t(P[1]) << 8))) / 32768.0;
			}
			else
			{
				const uint32_t Raw = uint32_t(P[0]) | (uint32_t(P[1]) << 8)
					| (uint32_t(P[2]) << 16) | (uint32_t(P[3]) << 24);
				Sum += double(int32_t(Raw)) / 2147483648.0;
			}
		}
		Out[IdxI] = (Channels == 1) ? Sum : Sum * Inv;
	}
	return true;
}

std::vector<double> ResampleLinear(const std::vector<double>& X, uint32_t SrcRate, uint32_t DstRate)
{
	if (SrcRate == DstRate || X.empty())
	{
		return X;
	}
	const size_t Count = size_t(double(X.size()) * double(DstRate) / double(SrcRate));
	const double Step = double(SrcRate) / double(DstRate);
	std::vector<double> Out(Count);
	for (size_t IdxI = 0; IdxI < Count; ++IdxI)
	{
		const double Pos = double(IdxI) * Step;
		const size_t J = size_t(Pos);
		const double Frac = Pos - double(J);
		const double A = X[J];
		const double B = (J + 1 < X.size()) ? X[J + 1] : A;
		Out[IdxI] = A + (B - A) * Frac;
	}
	return Out;
}

double Mean(const double* X, size_t N)
{
	if (N == 0)
	{
		return 0.0;
	}
	double Sum = 0.0;
#pragma omp simd reduction(+ : Sum)
	for (size_t IdxI = 0; IdxI < N; ++IdxI)
	{
		Sum += X[IdxI];
	}
	return Sum / double(N);
}

// Correlation of the equal-length prefixes; 0 when either side is flat, so a
// silent passage neither passes nor poisons a comparison.
double Pearson(const double* A, size_t Na, const double* B, size_t Nb)
{
	const size_t N = std::min(Na, Nb);
	if (N < 8)
	{
		return 0.0;
	}
	const double Ma = Mean(A, N), Mb = Mean(B, N);
	double Num = 0.0, Va = 0.0, Vb = 0.0;
#pragma omp simd reduction(+ : Num, Va, Vb)
	for (size_t IdxI = 0; IdxI < N; ++IdxI)
	{
		const double Da = A[IdxI] - Ma;
		const double Db = B[IdxI] - Mb;
		Num += Da * Db;
		Va += Da * Da;
		Vb += Db * Db;
	}
	if (Va <= 1e-12 || Vb <= 1e-12)
	{
		return 0.0;
	}
	return Num / sqrt(Va * Vb);
}

// Mean |x| per block - amplitude at about 86 Hz, and phase-free.
std::vector<double> Envelope(const double* X, size_t N, size_t Block)
{
	std::vector<double> Out;
	if (Block == 0 || N < Block)
	{
		return Out;
	}
	Out.reserve((N - Block) / Block + 1);
	for (size_t At = 0; At + Block <= N; At += Block)
	{
		double Sum = 0.0;
#pragma omp simd reduction(+ : Sum)
		for (size_t IdxI = 0; IdxI < Block; ++IdxI)
		{
			Sum += fabs(X[At + IdxI]);
		}
		Out.push_back(Sum / double(Block));
	}
	return Out;
}

// Iterative radix-2 FFT, in place; N must be a power of two.
void Fft(std::complex<double>* X, int N)
{
	for (int IdxI = 1, J = 0; IdxI < N; ++IdxI)
	{
		int Bit = N >> 1;
		for (; (J & Bit) != 0; Bit >>= 1)
		{
			J ^= Bit;
		}
		J |= Bit;
		if (IdxI < J)
		{
			std::swap(X[IdxI], X[J]);
		}
	}
	for (int Length = 2; Length <= N; Length <<= 1)
	{
		const double Angle = -2.0 * M_PI / double(Length);
		const std::complex<double> Step(cos(Angle), sin(Angle));
		const int Half = Length >> 1;
		for (int IdxI = 0; IdxI < N; IdxI += Length)
		{
			std::complex<double> W(1.0, 0.0);
			for (int K = IdxI; K < IdxI + Half; ++K)
			{
				const std::complex<double> U = X[K];
				const std::complex<double> V = X[K + Half] * W;
				X[K] = U + V;
				X[K + Half] = U - V;
				W *= Step;
			}
		}
	}
}

// Log-magnitude half-spectra, one row of kFftN/2 bins per hop.
struct Spectrogram
{
	std::vector<double> Rows; // Frames * Bins
	size_t Frames = 0, Bins = 0;

	const double* Row(size_t Index) const
	{
		return &Rows[Index * Bins];
	}
};

const std::vector<double>& HannWindow()
{
	static const std::vector<double> Window = []
	{
		std::vector<double> W(kFftN);
		for (int IdxI = 0; IdxI < kFftN; ++IdxI)
		{
			W[size_t(IdxI)] = 0.5 - 0.5 * cos(2.0 * M_PI * double(IdxI) / double(kFftN - 1));
		}
		return W;
	}();
	return Window;
}

Spectrogram Analyse(const double* X, size_t N)
{
	Spectrogram Out;
	Out.Bins = kFftN / 2;
	if (N < size_t(kFftN))
	{
		return Out;
	}
	const std::vector<double>& Hann = HannWindow();
	std::vector<std::complex<double>> Buffer(kFftN);
	for (size_t At = 0; At + kFftN <= N; At += kFftHop)
	{
		for (int IdxI = 0; IdxI < kFftN; ++IdxI)
		{
			Buffer[size_t(IdxI)] = std::complex<double>(X[At + size_t(IdxI)] * Hann[size_t(IdxI)], 0.0);
		}
		Fft(Buffer.data(), kFftN);
		const size_t Base = Out.Rows.size();
		Out.Rows.resize(Base + Out.Bins);
		for (size_t K = 0; K < Out.Bins; ++K)
		{
			Out.Rows[Base + K] = log1p(std::abs(Buffer[K]) * 100.0);
		}
		++Out.Frames;
	}
	return Out;
}

// Each spectrum collapsed to coarse bands, for cheap alignment.
Spectrogram BandReduce(const Spectrogram& In)
{
	Spectrogram Out;
	Out.Bins = kBands;
	Out.Frames = In.Frames;
	if (In.Frames == 0)
	{
		Out.Bins = 0;
		return Out;
	}
	const size_t Per = std::max<size_t>(1, In.Bins / kBands);
	Out.Rows.assign(In.Frames * kBands, 0.0);
	for (size_t F = 0; F < In.Frames; ++F)
	{
		const double* Src = In.Row(F);
		for (size_t B = 0; B < kBands; ++B)
		{
			double Sum = 0.0;
			for (size_t K = B * Per; K < (B + 1) * Per && K < In.Bins; ++K)
			{
				Sum += Src[K];
			}
			Out.Rows[F * kBands + B] = Sum;
		}
	}
	return Out;
}

// Mean per-frame correlation of two band spectrograms at frame shift K
// (positive = the test starts later than the reference).
double FrameSeqCorr(const Spectrogram& Ref, const Spectrogram& Test, long K)
{
	const long RefStart = (K >= 0) ? K : 0;
	const long TestStart = (K >= 0) ? 0 : -K;
	const long Pairs = std::min(long(Ref.Frames) - RefStart, long(Test.Frames) - TestStart);
	if (Pairs <= 0)
	{
		return 0.0;
	}
	double Sum = 0.0;
	for (long IdxI = 0; IdxI < Pairs; ++IdxI)
	{
		Sum += Pearson(Ref.Row(size_t(RefStart + IdxI)), Ref.Bins, Test.Row(size_t(TestStart + IdxI)),
			Test.Bins);
	}
	return Sum / double(Pairs);
}

// Alignment by spectral-sequence correlation. This works on material with no
// amplitude envelope at all - a constant-level chip tune - where an
// envelope-based search has nothing to lock onto.
void BestOffsetSpectral(const Spectrogram& Ref, const Spectrogram& Test, long MaxShiftFrames,
	long& OutShift, double& OutCorr)
{
	double BestR = -2.0;
	long BestK = 0;
	const long Limit = std::min(
		MaxShiftFrames, std::max(0L, long(std::min(Ref.Frames, Test.Frames)) / 2));
	for (long K = -Limit; K <= Limit; ++K)
	{
		const double R = FrameSeqCorr(Ref, Test, K);
		if (R > BestR || (R == BestR && labs(K) < labs(BestK)))
		{
			BestR = R;
			BestK = K;
		}
	}
	OutShift = BestK;
	OutCorr = BestR;
}

// A threshold echoed back the way the reference printed it: 0.75, not 0.750000.
std::string Number(double V)
{
	char Buf[64];
	if (V == floor(V) && fabs(V) < 1e15)
	{
		snprintf(Buf, sizeof(Buf), "%.1f", V);
	}
	else
	{
		snprintf(Buf, sizeof(Buf), "%g", V);
	}
	return Buf;
}

bool ParseDouble(const char* Text, double& Out)
{
	char* End = nullptr;
	const double V = strtod(Text, &End);
	if (End == Text || *End != '\0')
	{
		return false;
	}
	Out = V;
	return true;
}

} // namespace

int main(int Argc, char** Argv)
{
	std::string Ref, Test;
	double SkipS = 0.0, MaxShiftS = 5.0, MinSpec = 0.75, MinEnv = 0.6;
	bool bVerbose = false;
	std::vector<std::string> Positional;
	for (int IdxI = 1; IdxI < Argc; ++IdxI)
	{
		const std::string A = Argv[IdxI];
		auto Value = [&]() -> const char*
		{
			return (IdxI + 1 < Argc) ? Argv[++IdxI] : nullptr;
		};
		double* Dest = nullptr;
		if (A == "--skip-s")
		{
			Dest = &SkipS;
		}
		else if (A == "--max-shift-s")
		{
			Dest = &MaxShiftS;
		}
		else if (A == "--min-spec")
		{
			Dest = &MinSpec;
		}
		else if (A == "--min-env")
		{
			Dest = &MinEnv;
		}
		else if (A == "--verbose")
		{
			bVerbose = true;
			continue;
		}
		else if (A == "--help" || A == "-h")
		{
			puts("usage: wav-compare REF TEST [--skip-s S] [--max-shift-s S]\n"
				 "                   [--min-spec X] [--min-env X] [--verbose]");
			return 0;
		}
		else if (!A.empty() && A[0] == '-')
		{
			return Fail("unknown option " + A);
		}
		else
		{
			Positional.push_back(A);
			continue;
		}
		const char* V = Value();
		if (V == nullptr || !ParseDouble(V, *Dest))
		{
			return Fail(A + " needs a numeric value");
		}
	}
	if (Positional.size() != 2)
	{
		return Fail("a reference and a test WAV are required");
	}
	Ref = Positional[0];
	Test = Positional[1];

	if (!isfinite(SkipS) || SkipS < 0.0 || !isfinite(MaxShiftS) || MaxShiftS < 0.0)
	{
		return Fail("--skip-s and --max-shift-s must be finite and nonnegative");
	}
	if (!(isfinite(MinSpec) && MinSpec >= -1.0 && MinSpec <= 1.0)
		|| !(isfinite(MinEnv) && MinEnv >= -1.0 && MinEnv <= 1.0))
	{
		return Fail("correlation thresholds must be finite and in [-1, 1]");
	}

	std::vector<double> RefRaw, TestRaw;
	uint32_t RefRate = 0, TestRate = 0;
	std::string Error;
	if (!ReadWavMono(Ref, RefRaw, RefRate, Error) || !ReadWavMono(Test, TestRaw, TestRate, Error))
	{
		return Fail(Error);
	}
	std::vector<double> R = ResampleLinear(RefRaw, RefRate, kAnalysisRate);
	std::vector<double> T = ResampleLinear(TestRaw, TestRate, kAnalysisRate);
	const size_t Skip = size_t(std::max(0.0, SkipS * kAnalysisRate));
	R.erase(R.begin(), R.begin() + long(std::min(Skip, R.size())));
	T.erase(T.begin(), T.begin() + long(std::min(Skip, T.size())));
	if (R.size() < size_t(kFftN) || T.size() < size_t(kFftN))
	{
		return Fail("not enough audio after --skip-s");
	}

	printf("ref : %s  %u Hz  %.2f s (analysed)\n", Ref.c_str(), RefRate,
		double(R.size()) / kAnalysisRate);
	printf("test: %s  %u Hz  %.2f s (analysed)\n", Test.c_str(), TestRate,
		double(T.size()) / kAnalysisRate);

	// --- align on band spectrograms (envelope-free material still aligns) ---
	const double EnvRate = double(kAnalysisRate) / double(kEnvBlock);
	const Spectrogram RefBands = BandReduce(Analyse(R.data(), R.size()));
	const Spectrogram TestBands = BandReduce(Analyse(T.data(), T.size()));
	const long MaxShiftFrames = long(std::min(MaxShiftS, double(std::min(R.size(), T.size()))
												  / double(kAnalysisRate))
		* double(kAnalysisRate) / double(kFftHop));
	long ShiftFrames = 0;
	double AlignR = 0.0;
	BestOffsetSpectral(RefBands, TestBands, MaxShiftFrames, ShiftFrames, AlignR);
	const long Off = ShiftFrames * kFftHop;
	printf("alignment: test is %+.1f ms relative to ref (spectral seq r=%.3f at alignment, "
		   "resolution %.0f ms)\n",
		double(Off) / kAnalysisRate * 1000.0, AlignR,
		double(kFftHop) / kAnalysisRate * 1000.0);

	const double* RefAl = (Off >= 0) ? R.data() + Off : R.data();
	const double* TestAl = (Off >= 0) ? T.data() : T.data() - Off;
	size_t RefLeft = (Off >= 0) ? R.size() - size_t(std::min<long>(Off, long(R.size()))) : R.size();
	size_t TestLeft = (Off >= 0) ? T.size() : T.size() - size_t(std::min<long>(-Off, long(T.size())));
	const size_t N = std::min(RefLeft, TestLeft);
	RefLeft = TestLeft = N;

	// --- envelope correlation over the aligned overlap ---
	const std::vector<double> EnvRef = Envelope(RefAl, N, kEnvBlock);
	const std::vector<double> EnvTest = Envelope(TestAl, N, kEnvBlock);
	const double EnvR = Pearson(EnvRef.data(), EnvRef.size(), EnvTest.data(), EnvTest.size());
	// Does this material HAVE an envelope? Judged on a COARSE (100 ms)
	// envelope: at fine resolution a mix of square waves shows tone-phase
	// interference ripple that looks like variance but carries no musical
	// information (and even anti-correlates between renders with different
	// oscillator phases). Real dynamics - notes, drums, fades - survive
	// 100 ms averaging; interference ripple does not.
	const std::vector<double> Coarse = Envelope(RefAl, N, kAnalysisRate / 10);
	const double CoarseMean = Mean(Coarse.data(), Coarse.size());
	double Variance = 0.0;
	for (double V : Coarse)
	{
		Variance += (V - CoarseMean) * (V - CoarseMean);
	}
	const double EnvCv = (CoarseMean > 1e-9)
		? sqrt(Coarse.empty() ? 0.0 : Variance / double(Coarse.size())) / CoarseMean
		: 0.0;
	const bool bEnvUsable = EnvCv >= 0.1;

	// --- spectral correlation ---
	const Spectrogram SpecRef = Analyse(RefAl, N);
	const Spectrogram SpecTest = Analyse(TestAl, N);
	const size_t Frames = std::min(SpecRef.Frames, SpecTest.Frames);
	double SpecSum = 0.0;
	size_t Scored = 0;
	// Include steady tones as well as changing notes; a relative energy gate
	// would discard every frame of a constant-amplitude signal.
	for (size_t IdxI = 0; IdxI < Frames; ++IdxI)
	{
		double EnergyRef = 0.0, EnergyTest = 0.0;
		for (size_t K = 0; K < SpecRef.Bins; ++K)
		{
			EnergyRef += SpecRef.Row(IdxI)[K];
			EnergyTest += SpecTest.Row(IdxI)[K];
		}
		if (EnergyRef > 1e-12 || EnergyTest > 1e-12)
		{
			SpecSum += Pearson(SpecRef.Row(IdxI), SpecRef.Bins, SpecTest.Row(IdxI), SpecTest.Bins);
			++Scored;
		}
	}
	const double SpecR = (Scored != 0) ? SpecSum / double(Scored) : 1.0;

	// --- raw-waveform correlation (diagnostic) ---
	const size_t RawN = std::min<size_t>(N, size_t(kAnalysisRate) * 20);
	const double RawR = Pearson(RefAl, RawN, TestAl, RawN);

	// --- drift: local envelope re-alignment per 2 s window ---
	const long Win = bEnvUsable ? long(2.0 * EnvRate) : (1L << 30);
	const long Local = std::max(2L, long(0.05 * EnvRate)); // +-50 ms search
	std::vector<std::pair<double, double>> Points;
	const long Span = long(std::min(EnvRef.size(), EnvTest.size()));
	for (long W0 = 0; Win > 0 && W0 + Win <= Span; W0 += Win)
	{
		const double* Rw = EnvRef.data() + W0;
		const double* Tw = EnvTest.data() + W0;
		double BestR = Pearson(Rw, size_t(Win), Tw, size_t(Win));
		long BestK = 0;
		for (long K = -Local; K <= Local; ++K)
		{
			if (K == 0)
			{
				continue;
			}
			const double Lr = (K > 0) ? Pearson(Rw + K, size_t(Win - K), Tw, size_t(Win))
									  : Pearson(Rw, size_t(Win), Tw - K, size_t(Win + K));
			if (Lr > BestR)
			{
				BestR = Lr;
				BestK = K;
			}
		}
		if (BestR > 0.3)
		{
			Points.push_back({double(W0) / EnvRate, double(BestK) / EnvRate * 1000.0});
		}
		if (bVerbose)
		{
			printf("  win %6.2fs  env_r %+.3f  shift %+7.1f ms\n", double(W0) / EnvRate, BestR,
				double(BestK) / EnvRate * 1000.0);
		}
	}
	double Slope = 0.0;
	if (Points.size() >= 3)
	{
		double MeanT = 0.0, MeanS = 0.0;
		for (const std::pair<double, double>& P : Points)
		{
			MeanT += P.first;
			MeanS += P.second;
		}
		MeanT /= double(Points.size());
		MeanS /= double(Points.size());
		double Num = 0.0, Den = 0.0;
		for (const std::pair<double, double>& P : Points)
		{
			Num += (P.first - MeanT) * (P.second - MeanS);
			Den += (P.first - MeanT) * (P.first - MeanT);
		}
		Slope = (Den > 1e-9) ? Num / Den : 0.0;
	}

	double SqRef = 0.0, SqTest = 0.0;
#pragma omp simd reduction(+ : SqRef, SqTest)
	for (size_t IdxI = 0; IdxI < N; ++IdxI)
	{
		SqRef += RefAl[IdxI] * RefAl[IdxI];
		SqTest += TestAl[IdxI] * TestAl[IdxI];
	}
	const double RmsRef = sqrt(N != 0 ? SqRef / double(N) : 0.0);
	const double RmsTest = sqrt(N != 0 ? SqTest / double(N) : 0.0);
	const double Ratio = (RmsRef > 1e-9) ? RmsTest / RmsRef : INFINITY;

	printf("\nspectral corr : %.4f   (%zu frames scored)\n", SpecR, Scored);
	if (bEnvUsable)
	{
		printf("envelope corr : %.4f   (usable)\n", EnvR);
	}
	else
	{
		printf("envelope corr : %.4f   (NOT ENFORCED - flat envelope, cv=%.3f)\n", EnvR, EnvCv);
	}
	printf("raw-wave corr : %.4f   (diagnostic; phase-sensitive)\n", RawR);
	printf("rms level     : ref %.4f  test %.4f  ratio %.3f\n", RmsRef, RmsTest, Ratio);
	if (Points.size() >= 3)
	{
		printf("drift         : %+.3f ms/s (%s)\n", Slope,
			fabs(Slope) < 0.5 ? "small measured drift" : "possible timing drift");
	}
	else if (!bEnvUsable)
	{
		puts("drift         : skipped (flat envelope - no alignment signal)");
	}
	else
	{
		puts("drift         : not enough correlating windows to estimate");
	}

	const bool bOk = SpecR >= MinSpec && (EnvR >= MinEnv || !bEnvUsable);
	printf("\nverdict: %s (spectral %.3f vs %s, envelope %.3f vs %s%s)\n", bOk ? "PASS" : "FAIL",
		SpecR, Number(MinSpec).c_str(), EnvR, Number(MinEnv).c_str(),
		bEnvUsable ? "" : " [envelope not enforced]");
	return bOk ? 0 : 1;
}
